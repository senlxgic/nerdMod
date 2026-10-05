/*
	nerdMod Weather (weather.srldr). A BlocksDS application (DSWiFi in DSi mode + Mbed TLS) built in its own CI lane; see
	weather/README.md. Open-Meteo provides forecast and city search (no API key). The home menu only reads the cache file
	this app writes (sd:/_nds/nerdMod/cache/weather/weather.ini) and never touches the network.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fat.h>
#include <nds.h>

#include "common/nmweather.h"
#include "net.h"
#include "store.h"
#include "wdraw.h"

using namespace nmweather;

namespace {

u16 *topFb = nullptr, *botFb = nullptr;

void frame() { cothread_yield_irq(IRQ_VBLANK); }

void initVideo() {
	videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE);
	videoSetModeSub(MODE_5_2D | DISPLAY_BG3_ACTIVE);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);
	const int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	const int bgSub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	topFb = (u16 *)bgGetGfxPtr(bg);
	botFb = (u16 *)bgGetGfxPtr(bgSub);
}

// ---- input -------------------------------------------------------------------------------------------------------
struct Input {
	u32 down = 0;
	bool touched = false;
	int tx = 0, ty = 0;
};

Input poll() {
	Input in;
	scanKeys();
	in.down = keysDown();
	if (in.down & KEY_TOUCH) {
		touchPosition t;
		touchRead(&t);
		in.touched = true;
		in.tx = t.px;
		in.ty = t.py;
	}
	return in;
}

// ---- application state ---------------------------------------------------------------------------------------------
Settings settings;
Forecast forecast;
bool haveData = false;
char locationName[40] = "";
char countryName[28] = "";
char message[64] = "";
bool messageIsError = false;

void setMessage(const char *text, bool error) {
	snprintf(message, sizeof message, "%s", text);
	messageIsError = error;
}

void saveSettings() {
	char text[400];
	const int n = serializeSettings(text, sizeof text, settings);
	if (n > 0 && !store::writeFile(store::settingsPath(), text, (size_t)n))
		setMessage("Could not save settings (SD card?)", true);
}

void loadAll() {
	std::string text;
	if (store::readFile(store::settingsPath(), text, 2048))
		settings = parseSettings(text.c_str());
	if (store::readFile(store::cachePath(), text, 4096)) {
		Forecast f;
		if (parseFull(text.c_str(), f, locationName, sizeof locationName, countryName, sizeof countryName)) {
			forecast = f;
			haveData = true;
		}
	}
	// the cache must belong to the chosen city
	if (haveData && settings.place.valid && locationName[0] && strcmp(locationName, settings.place.name) != 0)
		haveData = false;
}

void saveCache() {
	char text[1500];
	const int n = serializeFull(text, sizeof text, forecast, settings.place.name, "open-meteo", settings.country);
	if (n > 0 && (size_t)n < sizeof text)
		store::writeFile(store::cachePath(), text, (size_t)n);
}

wd::MainView makeView(int selected) {
	wd::MainView v;
	v.placeSet = settings.place.valid;
	v.haveData = haveData;
	v.fc = forecast;
	snprintf(v.city, sizeof v.city, "%s", settings.place.name);
	snprintf(v.country, sizeof v.country, "%s", settings.country);
	v.fahrenheit = settings.fahrenheit;
	v.autoRefresh = settings.autoRefresh;
	v.intervalMin = settings.intervalMin;
	v.now = (int64_t)time(NULL);
	v.stale = haveData && isStale(v.now, forecast.now.time, settings.intervalMin);
	v.message = message;
	v.messageIsError = messageIsError;
	v.selected = selected;
	return v;
}

// ---- busy dialog with cancel -----------------------------------------------------------------------------------------
struct Busy {
	const char *title = "WEATHER";
	char status[64] = "";
	int frames = 0;
	bool bottomDrawn = false;
};

bool busyTick(void *ctx, const char *status) {
	Busy *b = (Busy *)ctx;
	const bool changed = strcmp(b->status, status) != 0;
	if (changed)
		snprintf(b->status, sizeof b->status, "%s", status);
	if (!b->bottomDrawn) {
		wd::renderBusyBottom(botFb);
		b->bottomDrawn = true;
	}
	if (changed || b->frames % 15 == 0)
		wd::renderBusy(topFb, b->title, b->status, b->frames, "B: CANCEL");
	b->frames++;
	frame();
	const Input in = poll();
	if (in.down & KEY_B)
		return false;
	if (in.touched && wd::inside(wd::backButton(), in.tx, in.ty))
		return false;
	return true;
}

// ---- refresh -------------------------------------------------------------------------------------------------------------
bool refresh() {
	if (!settings.place.valid) {
		setMessage("Choose a city first (tap CITY)", true);
		return false;
	}
	Busy busy;
	net::Info info;
	net::Err e = net::wifiUp(busyTick, &busy, info);
	std::string body;
	if (e == net::E_OK) {
		char url[400];
		forecastUrl(url, sizeof url, settings.place.lat, settings.place.lon);
		e = net::httpsGet(url, 12288, body, busyTick, &busy, info);
	}
	char buf[64];
	if (e != net::E_OK) {
		setMessage(net::shortText(e, info, buf, sizeof buf), e != net::E_CANCEL);
		return false;
	}
	Forecast f;
	if (!parseForecast(body.data(), body.size(), (int64_t)time(NULL), f)) {
		setMessage("Weather data unreadable - try again later", true);
		return false;
	}
	forecast = f;
	haveData = true;
	snprintf(locationName, sizeof locationName, "%s", settings.place.name);
	saveCache();
	setMessage("Updated", false);
	return true;
}

// ---- city search -------------------------------------------------------------------------------------------------------
PlaceList results;

// Returns the index of the chosen result, or -1 when the user went back.
int pickResult(const char *query) {
	int sel = 0;
	bool dirty = true;
	while (true) {
		if (dirty) {
			wd::renderResultsTop(topFb, results, sel, query);
			wd::renderResultsBottom(botFb, results, sel);
			dirty = false;
		}
		frame();
		const Input in = poll();
		const int rows = results.count < wd::RESULT_ROWS ? results.count : wd::RESULT_ROWS;
		if (in.down & KEY_B)
			return -1;
		if (in.touched) {
			if (wd::inside(wd::backButton(), in.tx, in.ty))
				return -1;
			const int hit = wd::hitResults(in.tx, in.ty, rows);
			if (hit >= 0)
				return hit;
		}
		if ((in.down & KEY_DOWN) && rows > 0) { sel = (sel + 1) % rows; dirty = true; }
		if ((in.down & KEY_UP) && rows > 0) { sel = (sel + rows - 1) % rows; dirty = true; }
		if ((in.down & KEY_A) && rows > 0)
			return sel;
	}
}

// True when a city was chosen (settings updated).
bool chooseCity() {
	char query[28] = "";
	char hint[64] = "";
	bool busyHint = false;
	wd::Key keys[wd::KB_MAX_KEYS];
	const int nkeys = wd::keyboardKeys(keys);
	wd::Rect rects[wd::KB_MAX_KEYS];
	for (int i = 0; i < nkeys; i++)
		rects[i] = keys[i].r;
	int sel = 0;
	bool dirty = true;
	while (true) {
		if (dirty) {
			wd::renderKeyboardTop(topFb, query, hint, busyHint);
			wd::renderKeyboardBottom(botFb, sel);
			dirty = false;
		}
		frame();
		const Input in = poll();
		wd::Button press = wd::BTN_NONE;
		if (in.touched)
			press = wd::hitKeyboard(in.tx, in.ty);
		if (in.down & KEY_A)
			press = keys[sel].id;
		if (in.down & KEY_B)
			press = wd::KB_BKSP;
		if (in.down & KEY_START)
			press = wd::KB_SEARCH;
		if (in.down & KEY_SELECT)
			press = wd::KB_CANCEL;
		if (in.down & KEY_RIGHT) { sel = wd::navigate(rects, nkeys, sel, 1, 0); dirty = true; }
		if (in.down & KEY_LEFT) { sel = wd::navigate(rects, nkeys, sel, -1, 0); dirty = true; }
		if (in.down & KEY_DOWN) { sel = wd::navigate(rects, nkeys, sel, 0, 1); dirty = true; }
		if (in.down & KEY_UP) { sel = wd::navigate(rects, nkeys, sel, 0, -1); dirty = true; }
		if (press == wd::BTN_NONE)
			continue;
		const size_t len = strlen(query);
		if (press >= 'A' && press <= 'Z') {
			if (len + 1 < sizeof query) { query[len] = (char)press; query[len + 1] = 0; }
			hint[0] = 0;
		} else if (press == wd::KB_SPACE) {
			if (len > 0 && query[len - 1] != ' ' && len + 1 < sizeof query) { query[len] = ' '; query[len + 1] = 0; }
		} else if (press == wd::KB_BKSP) {
			if (len > 0)
				query[len - 1] = 0;
			else if (in.down & KEY_B)
				return false; // B on an empty box leaves
			hint[0] = 0;
		} else if (press == wd::KB_CANCEL) {
			return false;
		} else if (press == wd::KB_SEARCH) {
			// trim trailing spaces
			size_t l = strlen(query);
			while (l > 0 && query[l - 1] == ' ')
				query[--l] = 0;
			if (l < 2) {
				snprintf(hint, sizeof hint, "TYPE AT LEAST 2 LETTERS");
				busyHint = false;
				dirty = true;
				continue;
			}
			Busy busy;
			busy.title = "SEARCHING";
			net::Info info;
			net::Err e = net::wifiUp(busyTick, &busy, info);
			std::string body;
			if (e == net::E_OK) {
				char url[300];
				geocodeUrl(url, sizeof url, query);
				e = net::httpsGet(url, 12288, body, busyTick, &busy, info);
			}
			busyHint = false;
			if (e != net::E_OK) {
				net::shortText(e, info, hint, sizeof hint); // (returns a literal for most errors)
				char buf[64];
				snprintf(hint, sizeof hint, "%s", net::shortText(e, info, buf, sizeof buf));
				dirty = true;
				continue;
			}
			if (!parseGeocode(body.data(), body.size(), results)) {
				snprintf(hint, sizeof hint, "SEARCH ANSWER UNREADABLE");
				dirty = true;
				continue;
			}
			if (results.count == 0) {
				snprintf(hint, sizeof hint, "NO PLACE FOUND - TRY ANOTHER NAME");
				dirty = true;
				continue;
			}
			const int pick = pickResult(query);
			if (pick < 0) {
				dirty = true;
				continue;
			}
			const Place &p = results.p[pick];
			Settings s = settings;
			s.place.valid = true;
			s.place.lat = p.lat;
			s.place.lon = p.lon;
			snprintf(s.place.name, sizeof s.place.name, "%s", p.name);
			snprintf(s.country, sizeof s.country, "%s", p.country);
			snprintf(s.timezone, sizeof s.timezone, "%s", p.timezone);
			settings = s;
			saveSettings();
			haveData = false; // the old forecast belongs to another place
			remove(store::cachePath().c_str());
			return true;
		}
		dirty = true;
	}
}

// ---- network test ---------------------------------------------------------------------------------------------------------
void networkTest() {
	while (true) {
		wd::TestLine lines[6];
		memset(lines, 0, sizeof lines);
		const char *names[6] = {"WI-FI", "IP ADDRESS", "DNS", "SECURE LINK", "WEATHER DATA", "CLOCK"};
		for (int i = 0; i < 6; i++) {
			snprintf(lines[i].name, sizeof lines[i].name, "%s", names[i]);
			snprintf(lines[i].result, sizeof lines[i].result, "not run");
		}
		wd::renderNetTestBottom(botFb);
		const int64_t now = (int64_t)time(NULL);
		{
			time_t t = (time_t)now;
			struct tm *tmv = gmtime(&t);
			if (tmv)
				snprintf(lines[5].result, sizeof lines[5].result, "%04d-%02d-%02d %02d:%02d %s", tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min, clockLooksSet(now) ? "" : "(WRONG?)");
			lines[5].state = clockLooksSet(now) ? 1 : 2;
		}
		lines[0].state = 3;
		wd::renderNetTestTop(topFb, lines, 6, "Testing... (B cancels)", true);

		Busy busy;
		busy.title = "NETWORK TEST";
		net::Info info;
		net::Err e = net::wifiUp(busyTick, &busy, info);
		std::string body;
		if (e == net::E_OK) {
			lines[0].state = 1;
			snprintf(lines[0].result, sizeof lines[0].result, "CONNECTED (DSI MODE REQUESTED)");
			lines[1].state = info.ip[0] ? 1 : 2;
			snprintf(lines[1].result, sizeof lines[1].result, "%s", info.ip[0] ? info.ip : "none");
			const double lat = settings.place.valid ? settings.place.lat : 52.52, lon = settings.place.valid ? settings.place.lon : 13.41;
			char url[400];
			forecastUrl(url, sizeof url, lat, lon);
			e = net::httpsGet(url, 12288, body, busyTick, &busy, info);
			if (info.serverIp[0]) {
				lines[2].state = 1;
				snprintf(lines[2].result, sizeof lines[2].result, "api.open-meteo.com %s", info.serverIp);
			} else if (e != net::E_CANCEL) {
				lines[2].state = e == net::E_CLOCK ? 0 : 2;
				snprintf(lines[2].result, sizeof lines[2].result, e == net::E_CLOCK ? "skipped (clock)" : "lookup failed");
			}
			if (info.tls[0]) {
				lines[3].state = 1;
				snprintf(lines[3].result, sizeof lines[3].result, "%s CERT VERIFIED", info.tls);
			} else if (e == net::E_CERT) {
				lines[3].state = 2;
				snprintf(lines[3].result, sizeof lines[3].result, "%s", info.verify[0] ? info.verify : "certificate refused");
			} else if (e == net::E_TLS || e == net::E_TIMEOUT) {
				lines[3].state = 2;
				snprintf(lines[3].result, sizeof lines[3].result, "handshake failed (%d)", info.tlsError);
			}
			if (e == net::E_OK) {
				Forecast f;
				const bool ok = parseForecast(body.data(), body.size(), now, f);
				lines[4].state = ok ? 1 : 2;
				snprintf(lines[4].result, sizeof lines[4].result, "HTTP %d, %d BYTES%s", info.httpStatus, info.bytes, ok ? "" : " (UNREADABLE)");
			} else if (info.httpStatus) {
				lines[4].state = 2;
				snprintf(lines[4].result, sizeof lines[4].result, "HTTP %d", info.httpStatus);
			}
		} else if (e != net::E_CANCEL) {
			lines[0].state = 2;
			snprintf(lines[0].result, sizeof lines[0].result, "%s", e == net::E_NO_AP ? "no access point / cannot connect" : e == net::E_TIMEOUT ? "timed out" : "Wi-Fi did not start");
		}
		char buf[64];
		(void)buf;
		wd::renderNetTestTop(topFb, lines, 6, net::longText(e), e == net::E_OK);
		while (true) {
			frame();
			const Input in = poll();
			if (in.down & KEY_B)
				return;
			if (in.touched && wd::inside(wd::backButton(), in.tx, in.ty))
				return;
			if ((in.down & KEY_A) || (in.touched && wd::inside(wd::retryButton(), in.tx, in.ty)))
				break;
		}
	}
}

} // namespace

int main(int, char **) {
	defaultExceptionHandler();
	initVideo();
	wd::renderBusy(topFb, "WEATHER", "Starting", 0, "");
	wd::renderBusyBottom(botFb);

	const bool sdOk = store::init();
	if (sdOk)
		loadAll();
	else
		setMessage("No SD card - settings cannot be saved", true);

	if (sdOk && settings.place.valid && settings.autoRefresh && (!haveData || isStale((int64_t)time(NULL), forecast.now.time, settings.intervalMin)))
		refresh();
	else if (!settings.place.valid)
		setMessage("Tap CITY to choose where you are", false);

	wd::Btn btns[wd::MAIN_BTN_COUNT];
	wd::mainButtons(btns);
	wd::Rect rects[wd::MAIN_BTN_COUNT];
	for (int i = 0; i < wd::MAIN_BTN_COUNT; i++)
		rects[i] = btns[i].r;
	int sel = 1; // REFRESH
	bool dirty = true;
	int idleFrames = 0;
	bool running = true;

	while (running) {
		if (dirty) {
			const wd::MainView v = makeView(sel);
			wd::renderMainTop(topFb, v);
			wd::renderMainBottom(botFb, v);
			dirty = false;
		}
		frame();
		const Input in = poll();
		wd::Button press = wd::BTN_NONE;
		if (in.touched)
			press = wd::hitMain(in.tx, in.ty);
		if (in.down & KEY_A)
			press = btns[sel].id;
		if (in.down & KEY_B)
			press = wd::BTN_BACK;
		if (in.down & KEY_RIGHT) { sel = wd::navigate(rects, wd::MAIN_BTN_COUNT, sel, 1, 0); dirty = true; }
		if (in.down & KEY_LEFT) { sel = wd::navigate(rects, wd::MAIN_BTN_COUNT, sel, -1, 0); dirty = true; }
		if (in.down & KEY_DOWN) { sel = wd::navigate(rects, wd::MAIN_BTN_COUNT, sel, 0, 1); dirty = true; }
		if (in.down & KEY_UP) { sel = wd::navigate(rects, wd::MAIN_BTN_COUNT, sel, 0, -1); dirty = true; }

		switch (press) {
			case wd::BTN_CITY:
				if (chooseCity()) {
					message[0] = 0;
					refresh();
				}
				dirty = true;
				break;
			case wd::BTN_REFRESH: refresh(); dirty = true; break;
			case wd::BTN_UNITS: settings.fahrenheit = !settings.fahrenheit; saveSettings(); dirty = true; break;
			case wd::BTN_AUTO: settings.autoRefresh = !settings.autoRefresh; saveSettings(); dirty = true; break;
			case wd::BTN_INTERVAL: {
				const int order[4] = {30, 60, 180, 360};
				int i = 0;
				while (i < 4 && order[i] != settings.intervalMin) i++;
				settings.intervalMin = order[(i + 1) % 4];
				saveSettings();
				dirty = true;
				break;
			}
			case wd::BTN_NETWORK: networkTest(); dirty = true; break;
			case wd::BTN_BACK: running = false; break;
			default: break;
		}

		// the "updated X ago" text and stale colour follow the clock: redraw once a minute
		if (++idleFrames >= 60 * 60) {
			idleFrames = 0;
			dirty = true;
		}
	}

	net::wifiDown();
	return 0; // back to the TWiLight menu (libnds returns to the loader that started us)
}
