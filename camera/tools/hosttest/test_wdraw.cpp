// Host test of the Weather app drawing/layout (weather/source/wdraw.h): layout invariants, hit tests, and an optional preview.
//   ./test_wdraw            checks
//   ./test_wdraw out_dir    also writes PPM pictures of every screen
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "../../../weather/source/wdraw.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace wd;

static void ppm(const char *dir, const char *name, const px *b) {
	char path[256];
	snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) { fputc(chR(b[i]), f); fputc(chG(b[i]), f); fputc(chB(b[i]), f); }
	fclose(f);
}

static bool overlap(const Rect &a, const Rect &b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
static bool onScreen(const Rect &r) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H; }

static nmweather::Forecast sample(nmweather::Condition c, bool day, int64_t &now) {
	using namespace nmweather;
	static const char *FORECAST =
		"{\"utc_offset_seconds\":18000,\"timezone\":\"Asia/Karachi\",\"current\":{\"time\":\"2025-10-09T08:15\",\"temperature_2m\":31.4,\"weather_code\":1,\"is_day\":1},"
		"\"daily\":{\"time\":[\"2025-10-09\",\"2025-10-10\",\"2025-10-11\",\"2025-10-12\",\"2025-10-13\",\"2025-10-14\",\"2025-10-15\"],"
		"\"weather_code\":[1,2,3,61,80,95,0],\"temperature_2m_max\":[34.1,33.0,32.2,29.9,28.0,30.5,35.0],\"temperature_2m_min\":[22.0,21.5,21.0,20.4,19.9,20.2,22.8],"
		"\"precipitation_probability_max\":[0,5,10,60,70,80,null]}}";
	Forecast f;
	now = daysFromCivil(2025, 10, 9) * 86400 + 4 * 3600;
	parseForecast(FORECAST, strlen(FORECAST), now, f);
	f.now.cond = c;
	f.isDay = day;
	return f;
}

int main(int argc, char **argv) {
	// ---- layout invariants
	{
		Btn m[MAIN_BTN_COUNT];
		mainButtons(m);
		for (int i = 0; i < MAIN_BTN_COUNT; i++) {
			CHECK(onScreen(m[i].r) && m[i].r.w >= 40 && m[i].r.h >= 24); // finger sized
			for (int j = i + 1; j < MAIN_BTN_COUNT; j++) CHECK(!overlap(m[i].r, m[j].r));
		}
		for (int i = 0; i < MAIN_BTN_COUNT; i++) CHECK(hitMain(m[i].r.x + m[i].r.w / 2, m[i].r.y + m[i].r.h / 2) == m[i].id);
		CHECK(hitMain(0, 0) == BTN_NONE && hitMain(255, 191) == BTN_NONE);
		Key k[KB_MAX_KEYS];
		const int n = keyboardKeys(k);
		CHECK(n == 26 + 4 && n <= KB_MAX_KEYS);
		for (int i = 0; i < n; i++) {
			CHECK(onScreen(k[i].r) && k[i].r.w >= 24 && k[i].r.h >= 24);
			for (int j = i + 1; j < n; j++) CHECK(!overlap(k[i].r, k[j].r));
			CHECK(hitKeyboard(k[i].r.x + 2, k[i].r.y + 2) == k[i].id);
		}
		int letters = 0;
		for (int i = 0; i < n; i++) if (k[i].ch >= 'A' && k[i].ch <= 'Z') letters++;
		CHECK(letters == 26);
		for (int i = 0; i < RESULT_ROWS; i++) {
			CHECK(onScreen(resultRow(i)) && !overlap(resultRow(i), backButton()));
			for (int j = i + 1; j < RESULT_ROWS; j++) CHECK(!overlap(resultRow(i), resultRow(j)));
			const Rect r = resultRow(i);
			CHECK(hitResults(r.x + 3, r.y + 3, RESULT_ROWS) == i);
		}
		CHECK(hitResults(0, 0, 3) == -1 && hitResults(10, resultRow(4).y + 3, 3) == -1); // rows beyond the list are not hit
		CHECK(inside(backButton(), 10, 170) && !inside(retryButton(), 10, 170));
	}

	// ---- rendering never writes outside the buffer and never crashes (guard bands around the buffers)
	{
		std::vector<px> mem(W * H + 2 * 4096, 0xABCD);
		px *b = mem.data() + 4096;
		int64_t now;
		const nmweather::Condition conds[] = {nmweather::COND_CLEAR, nmweather::COND_PARTLY, nmweather::COND_CLOUDY, nmweather::COND_FOG, nmweather::COND_RAIN, nmweather::COND_STORM, nmweather::COND_SNOW, nmweather::COND_UNKNOWN};
		for (bool day : {true, false})
			for (nmweather::Condition c : conds) {
				MainView v;
				v.placeSet = true; v.haveData = true; v.fc = sample(c, day, now); v.now = now; snprintf(v.city, sizeof v.city, "Lahore"); snprintf(v.country, sizeof v.country, "Pakistan");
				renderMainTop(b, v);
				if (argc > 1) {
					char name[40];
					snprintf(name, sizeof name, "main_top_%s_%s", nmweather::conditionName(c), day ? "day" : "night");
					if (day || c == nmweather::COND_CLEAR) ppm(argv[1], name, b);
				}
				renderMainBottom(b, v);
			}
		for (int i = 0; i < 4096; i++) { CHECK(mem[i] == 0xABCD); CHECK(mem[4096 + W * H + i] == 0xABCD); }
		// edge cases: very long names, fahrenheit, no data, no city, errors
		MainView v;
		v.placeSet = true; v.haveData = true; v.fc = sample(nmweather::COND_RAIN, true, now); v.now = now + 3 * 86400; v.stale = true; v.fahrenheit = true;
		strncpy(v.city, "Llanfairpwllgwyngyllgogerychwyrndrobwllllantysiliogogogoch", sizeof v.city - 1);
		strncpy(v.country, "United Kingdom of Great Britain and Northern", sizeof v.country - 1);
		v.message = "No Wi-Fi: set up an access point in DSi System Settings"; v.messageIsError = true; v.selected = 2;
		renderMainTop(b, v);
		if (argc > 1) { ppm(argv[1], "main_top_stale", b); }
		renderMainBottom(b, v);
		renderMainBottom(b, v);
		if (argc > 1) ppm(argv[1], "main_bottom_error", b);
		MainView empty;
		renderMainTop(b, empty);
		if (argc > 1) ppm(argv[1], "main_top_empty", b);
		renderMainBottom(b, empty);
		if (argc > 1) ppm(argv[1], "main_bottom_empty", b);
		MainView nocity; nocity.placeSet = false; renderMainTop(b, nocity); renderMainBottom(b, nocity);
		MainView fresh; fresh.placeSet = true; fresh.haveData = true; fresh.fc = sample(nmweather::COND_PARTLY, true, now); fresh.now = now; snprintf(fresh.city, sizeof fresh.city, "Lahore");
		snprintf(fresh.country, sizeof fresh.country, "Pakistan");
		renderMainBottom(b, fresh);
		if (argc > 1) { ppm(argv[1], "main_bottom", b); renderMainTop(b, fresh); ppm(argv[1], "main_top", b); }
		for (int i = 0; i < 4096; i++) { CHECK(mem[i] == 0xABCD); CHECK(mem[4096 + W * H + i] == 0xABCD); }

		renderKeyboardTop(b, "New Yor", "Searching...", true);
		if (argc > 1) ppm(argv[1], "kb_top", b);
		renderKeyboardBottom(b, 5);
		if (argc > 1) ppm(argv[1], "kb_bottom", b);
		renderKeyboardTop(b, "A very long city name that overflows the box for sure", "x", false);
		nmweather::PlaceList pl;
		const char *geo = "{\"results\":[{\"name\":\"Lahore\",\"latitude\":31.558,\"longitude\":74.35,\"country\":\"Pakistan\",\"admin1\":\"Punjab\",\"timezone\":\"Asia/Karachi\"},"
						  "{\"name\":\"Lahore\",\"latitude\":31.7,\"longitude\":73.9,\"country\":\"Pakistan\",\"admin1\":\"Punjab\"},{\"name\":\"New Lahore Colony Of Long Names\",\"latitude\":1,\"longitude\":2,\"country\":\"United States\",\"admin1\":\"Texas\"}]}";
		nmweather::parseGeocode(geo, strlen(geo), pl);
		renderResultsTop(b, pl, 0, "Lahore"); if (argc > 1) ppm(argv[1], "res_top", b);
		renderResultsBottom(b, pl, 0); if (argc > 1) ppm(argv[1], "res_bottom", b);
		nmweather::PlaceList none; renderResultsBottom(b, none, -1); renderResultsTop(b, none, -1, "zzzz");
		TestLine tl[6] = {{"WI-FI", "Connected (DSi mode)", 1}, {"IP ADDRESS", "192.168.1.23", 1}, {"DNS", "api.open-meteo.com = 1.2.3.4", 1}, {"SECURE LINK", "Certificate not trusted", 2}, {"HTTP", "not run", 0}, {"CLOCK", "2026-10-05 12:00", 3}};
		renderNetTestTop(b, tl, 6, "Server certificate not trusted by this app", false);
		if (argc > 1) ppm(argv[1], "net_top", b);
		renderNetTestBottom(b); if (argc > 1) ppm(argv[1], "net_bottom", b);
		renderBusy(b, "WEATHER", "Connecting to Wi-Fi...", 40, "B: cancel");
		if (argc > 1) ppm(argv[1], "busy", b);
		for (int i = 0; i < 4096; i++) { CHECK(mem[i] == 0xABCD); CHECK(mem[4096 + W * H + i] == 0xABCD); }
	}

	// ---- D-pad navigation reaches every key / button
	{
		Key k[KB_MAX_KEYS];
		const int n = keyboardKeys(k);
		Rect r[KB_MAX_KEYS];
		for (int i = 0; i < n; i++) r[i] = k[i].r;
		// from every key, every other key is reachable by following moves (no dead ends)
		for (int start = 0; start < n; start++) {
			bool seen[KB_MAX_KEYS] = {false};
			int stack[KB_MAX_KEYS], sp = 0;
			stack[sp++] = start; seen[start] = true;
			int reached = 1;
			while (sp) {
				const int c = stack[--sp];
				const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (auto &d : dirs) {
					const int nx = navigate(r, n, c, d[0], d[1]);
					if (nx >= 0 && !seen[nx]) { seen[nx] = true; stack[sp++] = nx; reached++; }
				}
			}
			CHECK(reached == n);
		}
		CHECK(navigate(r, n, 0, 1, 0) == 1);			// Q -> W
		CHECK(navigate(r, n, 0, -1, 0) == 0);			// nothing left of Q
		CHECK(navigate(r, n, 0, 0, 1) == 10);			// Q -> A (the key below)
		Btn m[MAIN_BTN_COUNT]; Rect mr[MAIN_BTN_COUNT];
		mainButtons(m);
		for (int i = 0; i < MAIN_BTN_COUNT; i++) mr[i] = m[i].r;
		for (int start = 0; start < MAIN_BTN_COUNT; start++) {
			bool seen[MAIN_BTN_COUNT] = {false};
			int stack[MAIN_BTN_COUNT], sp = 0, reached = 1;
			stack[sp++] = start; seen[start] = true;
			while (sp) {
				const int c = stack[--sp];
				const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (auto &d : dirs) { const int nx = navigate(mr, MAIN_BTN_COUNT, c, d[0], d[1]); if (nx >= 0 && !seen[nx]) { seen[nx] = true; stack[sp++] = nx; reached++; } }
			}
			CHECK(reached == MAIN_BTN_COUNT);
		}
	}

	// ---- fit() never overflows and shortens with ".."
	{
		char o[24];
		fit("Short", 2, 100, o, sizeof o); CHECK(!strcmp(o, "Short"));
		fit("A very long place name indeed", 1, 60, o, sizeof o); CHECK(textW(o, 1) <= 60 && strstr(o, "..") && strlen(o) < sizeof o);
		fit("X", 5, 1, o, sizeof o); CHECK(textW(o, 5) <= 1 || o[0] == 0);
	}

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("wdraw tests OK\n");
	return 0;
}
