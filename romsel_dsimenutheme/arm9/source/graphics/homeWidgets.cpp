#include "homeWidgets.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../playStats.h"
#include "ThemeConfig.h"
#include "ThemeTextures.h"
#include "common/inifile.h"
#include "common/nmfont.h"
#include "common/nmformat.h"
#include "common/nmnet.h"
#include "common/nmweather.h"
#include "common/systemdetails.h"
#include "common/twlmenusettings.h"
#include "gameArt.h"

extern u16 *colorTable;
extern uint photoWidth, photoHeight;

namespace homeWidgets {
namespace {

constexpr int PX = CARD_W * CARD_H;

inline u16 rgb(int r, int g, int b) { return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | BIT(15)); }

const u16 WHITE = rgb(255, 255, 255);
const u16 SOFT = rgb(200, 224, 245);
const u16 DIM = rgb(150, 178, 205);

// ---- state ---------------------------------------------------------------------------------------------
bool dirty = true;
bool leftShown = false, rightShown = false;
u32 leftSig = 0, rightSig = 0;

// selected game (left card shows its stats instead of the global ones)
bool selActive = false;
char selName[96] = "";
uint32_t selLaunches = 0, selSeconds = 0;
int64_t selLast = 0;
u32 selVersion = 0;

// weather
nmweather::Data wx;
nmweather::Location place;
bool wxLoaded = false;
enum WxState { WX_IDLE, WX_NO_PLACE, WX_UPDATING, WX_OFFLINE, WX_FRESH, WX_FAILED } wxState = WX_IDLE;
int wxFrames = 0;
nmnet::UnavailableTransport transport; // no Wi-Fi driver for DSi mode in this toolchain (docs/WEATHER.md)
nmnet::Request request(transport);

std::string rootDir() { return std::string(sys().isRunFromSD() ? "sd" : "fat") + ":/_nds/nerdMod"; }
std::string cacheDir() { return rootDir() + "/cache/weather"; }
std::string cachePath() { return cacheDir() + "/weather.ini"; }

bool readText(const std::string &path, char *buf, size_t n) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	const size_t got = fread(buf, 1, n - 1, f);
	fclose(f);
	buf[got] = 0;
	return true;
}

void mkdirs() {
	const std::string root = sys().isRunFromSD() ? "sd:" : "fat:";
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir(rootDir().c_str(), 0777);
	mkdir((rootDir() + "/cache").c_str(), 0777);
	mkdir(cacheDir().c_str(), 0777);
}

void writeCache() {
	mkdirs();
	char buf[400];
	const int n = nmweather::serialize(buf, sizeof(buf), wx);
	if (n <= 0 || n >= (int)sizeof(buf))
		return;
	FILE *f = fopen(cachePath().c_str(), "wb");
	if (!f)
		return;
	fwrite(buf, 1, (size_t)n, f);
	fclose(f);
}

void loadWeatherOnce() {
	if (wxLoaded)
		return;
	wxLoaded = true;
	if (!sys().fatInitOk())
		return;
	char buf[600];
	if (readText(cachePath(), buf, sizeof(buf)))
		nmweather::parseCache(buf, wx);
	if (readText(rootDir() + "/weather.ini", buf, sizeof(buf)))
		nmweather::parseLocation(buf, place);
	wxState = place.valid ? WX_IDLE : WX_NO_PLACE;
}

// Background update: starts a request once, a few seconds after the menu is up, and is stepped one poll per frame.
// It never waits for anything. With no usable transport it ends at once in WX_OFFLINE and the cache stays as it is.
void weatherTick() {
	if (!ms().homeWeather || !wxLoaded)
		return;
	wxFrames++;
	if (wxState == WX_IDLE && wxFrames > 180 && place.valid) {
		char url[300];
		if (place.relayUrl[0])
			snprintf(url, sizeof(url), "%s%slat=%.4f&lon=%.4f", place.relayUrl, strchr(place.relayUrl, '?') ? "&" : "?", place.lat, place.lon);
		else
			snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code", place.lat, place.lon);
		request.start(url, (u32)(wxFrames * 16), 8000); // https:// is refused by nmnet (no TLS): honest "offline"
		wxState = WX_UPDATING;
		dirty = true;
	} else if (wxState == WX_UPDATING) {
		const nmnet::Request::Status s = request.poll((u32)(wxFrames * 16));
		if (!request.finished())
			return;
		if (s == nmnet::Request::DONE) {
			nmweather::Data d;
			if (nmweather::parseResponse(request.body().c_str(), (int64_t)time(NULL), d)) {
				snprintf(d.location, sizeof(d.location), "%s", place.name);
				snprintf(d.provider, sizeof(d.provider), "%s", place.relayUrl[0] ? "relay-v1" : "open-meteo");
				wx = d;
				writeCache();
				wxState = WX_FRESH;
			} else {
				wxState = WX_FAILED;
			}
		} else {
			wxState = WX_OFFLINE;
		}
		dirty = true;
	}
}

// ---- drawing helpers (card pixel buffer) -----------------------------------------------------------------
void fillRect(u16 *p, int x0, int y0, int w, int h, u16 c) {
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			if (x >= 0 && y >= 0 && x < CARD_W && y < CARD_H)
				p[y * CARD_W + x] = c;
}

void fillCircle(u16 *p, int cx, int cy, int r, u16 c) {
	for (int y = -r; y <= r; y++)
		for (int x = -r; x <= r; x++)
			if (x * x + y * y <= r * r + r / 2) {
				const int px = cx + x, py = cy + y;
				if (px >= 0 && py >= 0 && px < CARD_W && py < CARD_H)
					p[py * CARD_W + px] = c;
			}
}

void line(u16 *p, int x0, int y0, int x1, int y1, u16 c) {
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
	for (int guard = 0; guard < 200; guard++) {
		if (x0 >= 0 && y0 >= 0 && x0 < CARD_W && y0 < CARD_H)
			p[y0 * CARD_W + x0] = c;
		if (x0 == x1 && y0 == y1)
			break;
		const int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

void background(u16 *p, u16 top, u16 bottom) {
	const int tr = top & 31, tg = (top >> 5) & 31, tb = (top >> 10) & 31;
	const int br = bottom & 31, bg = (bottom >> 5) & 31, bb = (bottom >> 10) & 31;
	for (int y = 0; y < CARD_H; y++) {
		const int t = y * 256 / CARD_H;
		const u16 c = (u16)((tr + (br - tr) * t / 256) | ((tg + (bg - tg) * t / 256) << 5) | ((tb + (bb - tb) * t / 256) << 10) | BIT(15));
		for (int x = 0; x < CARD_W; x++)
			p[y * CARD_W + x] = c;
	}
	// glossy highlight on the upper third (a lighter band)
	for (int y = 1; y < CARD_H / 3; y++)
		for (int x = 1; x < CARD_W - 1; x++) {
			const u16 c = p[y * CARD_W + x];
			const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
			p[y * CARD_W + x] = (u16)((r + 2 > 31 ? 31 : r + 2) | ((g + 2 > 31 ? 31 : g + 2) << 5) | ((b + 2 > 31 ? 31 : b + 2) << 10) | BIT(15));
		}
	const u16 edge = rgb(225, 238, 250);
	for (int x = 1; x < CARD_W - 1; x++)
		p[x] = p[(CARD_H - 1) * CARD_W + x] = edge;
	for (int y = 1; y < CARD_H - 1; y++)
		p[y * CARD_W] = p[y * CARD_W + CARD_W - 1] = edge;
	const u16 dim = rgb(110, 150, 190); // corner pixels softened
	p[0] = p[CARD_W - 1] = p[(CARD_H - 1) * CARD_W] = p[(CARD_H - 1) * CARD_W + CARD_W - 1] = dim;
}

void cloud(u16 *p, int cx, int cy, u16 body, u16 shade) {
	fillCircle(p, cx - 6, cy + 2, 5, body);
	fillCircle(p, cx + 1, cy - 2, 7, body);
	fillCircle(p, cx + 8, cy + 2, 5, body);
	fillRect(p, cx - 8, cy + 2, 18, 5, body);
	fillRect(p, cx - 8, cy + 6, 18, 2, shade);
}

void sun(u16 *p, int cx, int cy, int r) {
	const u16 ray = rgb(255, 214, 80), core = rgb(255, 232, 110);
	static const int d[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
	for (int i = 0; i < 8; i++) {
		const int len0 = r + 2, len1 = r + 5;
		const int k0 = (d[i][0] && d[i][1]) ? len0 * 7 / 10 : len0, k1 = (d[i][0] && d[i][1]) ? len1 * 7 / 10 : len1;
		line(p, cx + d[i][0] * k0, cy + d[i][1] * k0, cx + d[i][0] * k1, cy + d[i][1] * k1, ray);
	}
	fillCircle(p, cx, cy, r, core);
}

// 27 x 22 icon area centred at (cx, cy)
void drawIcon(u16 *p, nmweather::Condition c, int cx, int cy) {
	const u16 grey = rgb(236, 242, 250), greyShade = rgb(170, 186, 208), dark = rgb(120, 134, 158), darkShade = rgb(80, 92, 116);
	switch (c) {
		case nmweather::COND_CLEAR:
			sun(p, cx, cy, 7);
			break;
		case nmweather::COND_PARTLY:
			sun(p, cx - 6, cy - 4, 5);
			cloud(p, cx + 2, cy + 3, grey, greyShade);
			break;
		case nmweather::COND_CLOUDY:
			cloud(p, cx, cy, grey, greyShade);
			break;
		case nmweather::COND_FOG:
			for (int i = 0; i < 4; i++)
				fillRect(p, cx - 11 + (i & 1) * 3, cy - 7 + i * 5, 20, 2, i & 1 ? greyShade : grey);
			break;
		case nmweather::COND_RAIN:
			cloud(p, cx, cy - 4, dark, darkShade);
			for (int i = 0; i < 3; i++)
				line(p, cx - 7 + i * 7, cy + 5, cx - 9 + i * 7, cy + 10, rgb(110, 190, 255));
			break;
		case nmweather::COND_STORM:
			cloud(p, cx, cy - 5, dark, darkShade);
			line(p, cx + 1, cy + 3, cx - 3, cy + 8, rgb(255, 224, 70));
			line(p, cx - 3, cy + 8, cx + 2, cy + 8, rgb(255, 224, 70));
			line(p, cx + 2, cy + 8, cx - 2, cy + 12, rgb(255, 224, 70));
			break;
		case nmweather::COND_SNOW:
			cloud(p, cx, cy - 4, grey, greyShade);
			for (int i = 0; i < 3; i++) {
				fillRect(p, cx - 8 + i * 7, cy + 5 + (i & 1) * 3, 2, 2, WHITE);
			}
			break;
		default:
			nmfont::draw(p, CARD_W, CARD_H, cx - 4, cy - 7, "?", 2, SOFT);
			break;
	}
}

void centredText(u16 *p, int y, const char *s, int scale, u16 colour) {
	// shrink to fit the card
	while (scale > 1 && nmfont::textWidth(s, scale) > CARD_W - 4)
		scale--;
	nmfont::drawCentred(p, CARD_W, CARD_H, y, s, scale, colour);
}

void upper(char *s) {
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 'a' + 'A');
}

// ---- cards -------------------------------------------------------------------------------------------------
// Splits "1h 32m" into "1H" / "32M"; "12 min" into "12" / "MIN"; "<1 min" into "<1" / "MIN".
void splitTime(const char *t, char *a, size_t an, char *b, size_t bn) {
	a[0] = b[0] = 0;
	if (const char *sp = strchr(t, ' ')) {
		snprintf(a, an, "%.*s", (int)(sp - t), t);
		snprintf(b, bn, "%s", sp + 1);
	} else {
		snprintf(a, an, "%s", t);
	}
	upper(a);
	upper(b);
}

u32 leftSignature() {
	const playstats::Global &g = playstats::global();
	if (selActive)
		return selVersion * 2654435761u + selLaunches * 31u + selSeconds * 7u + (u32)(selLast / 3600) + 1u;
	return g.totalSeconds * 31u + g.launches;
}

// Global card: "PLAY STATS", time played, launches. e.g. "1 min played / 30 launches", "1h 42m / 30 launches".
void renderStatsCard(u16 *p, u32 &sig) {
	const playstats::Global &g = playstats::global();
	background(p, rgb(50, 150, 215), rgb(20, 70, 140));
	centredText(p, 6, "PLAY", 1, SOFT);
	centredText(p, 15, "STATS", 1, SOFT);
	char t[24], a[16], b[16];
	nmformat::playTime(t, sizeof(t), g.totalSeconds);
	splitTime(t, a, sizeof(a), b, sizeof(b));
	const bool hours = strchr(t, 'h') != nullptr;
	if (hours) {
		centredText(p, 28, a, 2, WHITE); // "1H"
		centredText(p, 44, b, 2, WHITE); // "32M"
		snprintf(t, sizeof(t), "%lu", (unsigned long)g.launches);
		centredText(p, 63, t, 1, WHITE);
		centredText(p, 72, g.launches == 1 ? "LAUNCH" : "LAUNCHES", 1, DIM);
	} else {
		centredText(p, 29, a, 2, WHITE); // "12" or "<1"
		centredText(p, 45, b, 1, SOFT);	 // "MIN"
		snprintf(t, sizeof(t), "%lu", (unsigned long)g.launches);
		centredText(p, 59, t, 2, WHITE);
		centredText(p, 75, g.launches == 1 ? "LAUNCH" : "LAUNCHES", 1, DIM);
	}
	sig = leftSignature();
}

// Selected game card: title, "Played N times", total time, last played.
void renderGameCard(u16 *p, u32 &sig) {
	background(p, rgb(50, 150, 215), rgb(20, 70, 140));
	char lines[2][24];
	const int n = nmformat::wrapTitle(selName, 8, 2, lines);
	for (int i = 0; i < n; i++) {
		upper(lines[i]);
		centredText(p, 6 + i * 9, lines[i], 1, WHITE);
	}
	fillRect(p, 8, 26, CARD_W - 16, 1, rgb(150, 200, 235));
	char t[24];
	if (selLaunches == 0) {
		centredText(p, 32, "NOT PLAYED", 1, SOFT);
		centredText(p, 41, "YET", 1, SOFT);
	} else {
		centredText(p, 31, "PLAYED", 1, SOFT);
		snprintf(t, sizeof(t), "%lu %s", (unsigned long)selLaunches, selLaunches == 1 ? "TIME" : "TIMES");
		centredText(p, 40, t, 1, WHITE);
		if (selSeconds > 0) {
			nmformat::playTime(t, sizeof(t), selSeconds);
			upper(t);
			centredText(p, 53, t, 1, WHITE);
		}
		char day[24];
		nmformat::lastPlayedDay(day, sizeof(day), selLast, (int64_t)time(NULL));
		if (day[0]) {
			centredText(p, 65, "LAST", 1, DIM);
			upper(day);
			centredText(p, 74, day, 1, WHITE);
		}
	}
	sig = leftSignature();
}

u32 weatherSignature() {
	return (u32)(wx.valid ? (wx.tempC10 + 2000) * 7 + wx.cond * 3 + (u32)(wx.time / 60) : 1) * 131u + (u32)wxState * 17u + (ms().weatherFahrenheit ? 5u : 0u) + (place.valid ? 9u : 0u);
}

void renderWeatherCard(u16 *p, u32 &sig) {
	background(p, rgb(70, 165, 230), rgb(28, 95, 165));
	const int64_t now = (int64_t)time(NULL);
	if (wx.valid) {
		drawIcon(p, wx.cond, CARD_W / 2, 20);
		char t[16];
		nmweather::tempText(t, sizeof(t), wx.tempC10, ms().weatherFahrenheit);
		centredText(p, 38, t, 2, WHITE);
		char cn[16];
		snprintf(cn, sizeof(cn), "%s", nmweather::conditionName(wx.cond));
		upper(cn);
		centredText(p, 56, cn, 1, SOFT);
		char age[24];
		nmformat::ago(age, sizeof(age), wx.time, now);
		upper(age);
		if (age[0])
			centredText(p, 66, age, 1, (wxState == WX_FRESH) ? SOFT : DIM);
		if (wx.location[0]) {
			char loc[12];
			snprintf(loc, sizeof(loc), "%.8s", wx.location);
			upper(loc);
			centredText(p, 75, loc, 1, DIM);
		}
	} else {
		drawIcon(p, nmweather::COND_CLOUDY, CARD_W / 2, 20);
		centredText(p, 38, "--", 2, WHITE);
		centredText(p, 58, !place.valid ? "SET PLACE" : (wxState == WX_UPDATING ? "UPDATING" : "OFFLINE"), 1, SOFT);
	}
	sig = weatherSignature();
}

u16 mapColour(u16 c) { return colorTable ? (u16)(colorTable[c % 0x8000] | BIT(15)) : c; }

void blit(const u16 *card, int x0) {
	u16 *b1 = ThemeTextures::beginBgSubModify();
	u16 *b2 = boxArtColorDeband ? ThemeTextures::bgSubBuffer2() : nullptr;
	for (int y = 0; y < CARD_H; y++) {
		u16 *d1 = b1 + (CARD_Y + y) * 256 + x0;
		u16 *d2 = b2 ? b2 + (CARD_Y + y) * 256 + x0 : nullptr;
		for (int x = 0; x < CARD_W; x++) {
			const u16 c = mapColour(card[y * CARD_W + x]);
			d1[x] = c;
			if (d2)
				d2[x] = c;
		}
	}
	ThemeTextures::commitBgSubModify();
}

bool layoutActive() { return gameArt::enabled(); }

void drawLeft() {
	static u16 card[PX];
	u32 sig;
	if (selActive)
		renderGameCard(card, sig);
	else
		renderStatsCard(card, sig);
	blit(card, LEFT_X);
	leftShown = true;
	leftSig = sig;
}

void drawRight() {
	static u16 card[PX];
	u32 sig;
	renderWeatherCard(card, sig);
	blit(card, RIGHT_X);
	rightShown = true;
	rightSig = sig;
}

} // namespace

void invalidate() {
	dirty = true;
	leftShown = rightShown = false;
}

void setSelectedGame(const char *name, uint32_t launches, uint32_t seconds, int64_t lastPlayed) {
	snprintf(selName, sizeof(selName), "%s", name ? name : "");
	selLaunches = launches;
	selSeconds = seconds;
	selLast = lastPlayed;
	selVersion++;
	selActive = true;
}

void clearSelectedGame() {
	if (selActive)
		selVersion++;
	selActive = false;
}

void artCleared() {
	leftShown = false;
	dirty = true;
}

void tick() {
	if (!layoutActive())
		return;
	if (!sys().fatInitOk())
		return;
	playstats::init();
	loadWeatherOnce();
	weatherTick();

	const bool wantLeft = ms().homePlayStats && !gameArt::visible();
	const bool wantRight = ms().homeWeather;
	const bool leftStale = leftShown && wantLeft && leftSignature() != leftSig;
	if (!dirty && !leftStale && wantLeft == leftShown && wantRight == rightShown && (!rightShown || weatherSignature() == rightSig))
		return;
	dirty = false;
	// at most one card per call (each is a 4.5 KB blit); the next call finishes the job
	if (wantRight && (!rightShown || weatherSignature() != rightSig)) {
		drawRight();
		dirty = true;
		return;
	}
	if (wantLeft && (!leftShown || leftStale)) {
		drawLeft();
		return;
	}
	if (!wantLeft && leftShown)
		leftShown = false; // the art panel (or the setting) took the lane; restoring happens in gameArt::clear
}

} // namespace homeWidgets
