/*
	Weather app drawing and layout. Pure code: it draws into plain 256x192 RGB555 buffers (bit 15 set) and knows nothing
	about libnds, so the host tests can render every screen to a picture and check the touch layout.

	Look: a "Frutiger Aero" sky for the top screen (gradient, sun / moon, clouds, rain...) and glossy glass buttons on a
	light blue bottom screen. Text uses the 5x7 nmfont.
*/
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/nmfont.h"
#include "common/nmweather.h"

namespace wd {

constexpr int W = 256, H = 192;
typedef uint16_t px;

inline px rgb(int r, int g, int b) { return (px)(0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }
inline int chR(px c) { return (c & 31) << 3; }
inline int chG(px c) { return ((c >> 5) & 31) << 3; }
inline int chB(px c) { return ((c >> 10) & 31) << 3; }
// a * (256 - t) / 256 + b * t / 256
inline px mix(px a, px b, int t) {
	if (t <= 0) return a;
	if (t >= 256) return b;
	return rgb((chR(a) * (256 - t) + chR(b) * t) >> 8, (chG(a) * (256 - t) + chG(b) * t) >> 8, (chB(a) * (256 - t) + chB(b) * t) >> 8);
}

inline void put(px *buf, int x, int y, px c) {
	if (x >= 0 && y >= 0 && x < W && y < H)
		buf[y * W + x] = c;
}
inline void putA(px *buf, int x, int y, px c, int alpha) {
	if (x >= 0 && y >= 0 && x < W && y < H)
		buf[y * W + x] = mix(buf[y * W + x], c, alpha);
}

inline void fill(px *buf, px c) {
	for (int i = 0; i < W * H; i++)
		buf[i] = c;
}
inline void rect(px *buf, int x, int y, int w, int h, px c, int alpha = 256) {
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			putA(buf, i, j, c, alpha);
}
inline void gradient(px *buf, int y0, int y1, px top, px bottom) {
	for (int y = y0; y < y1 && y < H; y++) {
		const px c = mix(top, bottom, (y - y0) * 256 / (y1 - y0 > 1 ? y1 - y0 - 1 : 1));
		for (int x = 0; x < W; x++)
			buf[y * W + x] = c;
	}
}
inline void disc(px *buf, int cx, int cy, int r, px c, int alpha = 256) {
	for (int y = -r; y <= r; y++)
		for (int x = -r; x <= r; x++)
			if (x * x + y * y <= r * r)
				putA(buf, cx + x, cy + y, c, alpha);
}
inline void line(px *buf, int x0, int y0, int x1, int y1, px c, int thick = 1) {
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
	const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	for (int guard = 0; guard < 2000; guard++) {
		for (int j = 0; j < thick; j++)
			for (int i = 0; i < thick; i++)
				put(buf, x0 + i - thick / 2, y0 + j - thick / 2, c);
		if (x0 == x1 && y0 == y1)
			break;
		const int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

// Rounded rectangle with a vertical gradient from `c0` (top) to `c1` (bottom).
inline void roundGrad(px *buf, int x, int y, int w, int h, int r, px c0, px c1, int alpha = 256) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	for (int j = 0; j < h; j++) {
		int inset = 0;
		if (j < r) { const int dy = r - 1 - j; for (inset = 0; inset < r; inset++) if ((r - inset) * (r - inset) + dy * dy <= r * r) break; }
		else if (j >= h - r) { const int dy = j - (h - r); for (inset = 0; inset < r; inset++) if ((r - inset) * (r - inset) + dy * dy <= r * r) break; }
		const px c = mix(c0, c1, j * 256 / (h > 1 ? h - 1 : 1));
		for (int i = inset; i < w - inset; i++)
			putA(buf, x + i, y + j, c, alpha);
	}
}
inline void roundRect(px *buf, int x, int y, int w, int h, int r, px c, int alpha = 256) { roundGrad(buf, x, y, w, h, r, c, c, alpha); }

// ---- text ---------------------------------------------------------------------------------------------------------
inline int textW(const char *s, int scale) { return nmfont::textWidth(s, scale); }
inline void text(px *buf, int x, int y, const char *s, int scale, px c) { nmfont::draw(buf, W, H, x, y, s, scale, c); }
inline void textShadow(px *buf, int x, int y, const char *s, int scale, px c, px shadow) {
	nmfont::draw(buf, W, H, x + scale, y + scale, s, scale, shadow);
	nmfont::draw(buf, W, H, x, y, s, scale, c);
}
inline void textC(px *buf, int cx, int y, const char *s, int scale, px c, bool shadow = false) {
	const int x = cx - textW(s, scale) / 2;
	if (shadow) textShadow(buf, x, y, s, scale, c, rgb(20, 40, 70)); else text(buf, x, y, s, scale, c);
}
inline void textR(px *buf, int rx, int y, const char *s, int scale, px c) { text(buf, rx - textW(s, scale), y, s, scale, c); }
// Truncates `s` to what fits in `maxW` pixels (adds ".." when cut).
inline void fit(const char *s, int scale, int maxW, char *out, size_t cap) {
	{
		size_t l = strlen(s);
		if (l >= cap)
			l = cap - 1;
		memcpy(out, s, l);
		out[l] = 0;
	}
	if (textW(out, scale) <= maxW)
		return;
	size_t n = strlen(out);
	while (n > 0) {
		out[--n] = 0;
		char tmp[80];
		snprintf(tmp, sizeof tmp, "%s..", out);
		if (textW(tmp, scale) <= maxW) {
			size_t l = strlen(tmp);
			if (l >= cap)
				l = cap - 1;
			memcpy(out, tmp, l);
			out[l] = 0;
			return;
		}
	}
	out[0] = 0;
}

// ---- colours ------------------------------------------------------------------------------------------------------
struct Sky { px top, bottom; };
inline Sky skyFor(nmweather::Condition c, bool day) {
	using namespace nmweather;
	if (!day) {
		switch (c) {
			case COND_CLEAR: case COND_PARTLY: return {rgb(8, 18, 52), rgb(40, 70, 120)};
			case COND_STORM: return {rgb(18, 12, 38), rgb(60, 50, 90)};
			default: return {rgb(20, 28, 54), rgb(70, 84, 112)};
		}
	}
	switch (c) {
		case COND_CLEAR: return {rgb(28, 120, 220), rgb(150, 215, 250)};
		case COND_PARTLY: return {rgb(52, 140, 224), rgb(170, 220, 248)};
		case COND_CLOUDY: return {rgb(104, 134, 168), rgb(190, 208, 224)};
		case COND_FOG: return {rgb(150, 164, 178), rgb(214, 222, 228)};
		case COND_RAIN: return {rgb(64, 88, 120), rgb(140, 160, 184)};
		case COND_STORM: return {rgb(44, 40, 76), rgb(110, 110, 140)};
		case COND_SNOW: return {rgb(150, 174, 200), rgb(228, 238, 248)};
		default: return {rgb(90, 120, 160), rgb(170, 190, 210)};
	}
}

// ---- weather illustrations ------------------------------------------------------------------------------------------
inline void cloud(px *b, int cx, int cy, int s, px body, px shade) {
	// s = overall half width; three bumps on a flat base
	disc(b, cx - s * 45 / 100, cy + s * 5 / 100, s * 32 / 100, shade);
	disc(b, cx + s * 45 / 100, cy + s * 8 / 100, s * 28 / 100, shade);
	disc(b, cx, cy - s * 12 / 100, s * 42 / 100, shade);
	rect(b, cx - s * 45 / 100, cy + s * 8 / 100, s * 90 / 100, s * 29 / 100, shade);
	disc(b, cx - s * 45 / 100, cy + s * 2 / 100, s * 30 / 100, body);
	disc(b, cx + s * 45 / 100, cy + s * 4 / 100, s * 26 / 100, body);
	disc(b, cx, cy - s * 16 / 100, s * 40 / 100, body);
	rect(b, cx - s * 45 / 100, cy + s * 4 / 100, s * 90 / 100, s * 25 / 100, body);
}
inline void sun(px *b, int cx, int cy, int r, bool rays) {
	if (rays)
		for (int i = 0; i < 12; i++) {
			const int dx[12] = {100, 87, 50, 0, -50, -87, -100, -87, -50, 0, 50, 87}, dy[12] = {0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50};
			line(b, cx + dx[i] * r * 13 / 1000, cy + dy[i] * r * 13 / 1000, cx + dx[i] * r * 17 / 1000, cy + dy[i] * r * 17 / 1000, rgb(255, 214, 90), r >= 24 ? 3 : 2);
		}
	disc(b, cx, cy, r * 12 / 10, rgb(255, 240, 160), 90);
	disc(b, cx, cy, r, rgb(255, 196, 40));
	disc(b, cx - r / 4, cy - r / 4, r * 6 / 10, rgb(255, 232, 120), 200);
}
inline void moon(px *b, int cx, int cy, int r, px sky) {
	(void)sky;
	for (int y = -r; y <= r; y++)
		for (int x = -r; x <= r; x++) {
			if (x * x + y * y > r * r) continue;
			const int ox = x - r * 45 / 100, oy = y + r * 15 / 100;
			if (ox * ox + oy * oy <= (r * 85 / 100) * (r * 85 / 100)) continue; // cut-out makes the crescent
			put(b, cx + x, cy + y, rgb(250, 244, 205));
		}
}
inline void rain(px *b, int cx, int cy, int s, int drops, px c) {
	for (int i = 0; i < drops; i++) {
		const int x = cx - s * 40 / 100 + i * (s * 80 / 100) / (drops > 1 ? drops - 1 : 1);
		const int y = cy + s * 38 / 100 + (i % 2) * s * 12 / 100;
		line(b, x, y, x - s * 8 / 100, y + s * 22 / 100, c, s >= 40 ? 2 : 1);
	}
}
inline void bolt(px *b, int cx, int cy, int s) {
	const int x0 = cx, y0 = cy + s * 25 / 100;
	const int px_[6] = {x0 + s * 8 / 100, x0 - s * 12 / 100, x0 + s * 2 / 100, x0 - s * 14 / 100, x0 + s * 2 / 100, x0 - s * 6 / 100};
	const int py_[6] = {y0, y0 + s * 22 / 100, y0 + s * 22 / 100, y0 + s * 48 / 100, y0 + s * 28 / 100, y0 + s * 28 / 100};
	for (int i = 0; i < 5; i++)
		line(b, px_[i], py_[i], px_[i + 1], py_[i + 1], rgb(255, 224, 70), s >= 40 ? 3 : 2);
}
inline void snow(px *b, int cx, int cy, int s) {
	for (int i = 0; i < 5; i++) {
		const int x = cx - s * 40 / 100 + i * (s * 80 / 100) / 4, y = cy + s * 42 / 100 + (i % 2) * s * 14 / 100;
		disc(b, x, y, s >= 40 ? 3 : 2, rgb(255, 255, 255));
	}
}
inline void fogBars(px *b, int cx, int cy, int s, px c) {
	for (int i = 0; i < 4; i++)
		roundRect(b, cx - s * (70 - (i % 2) * 14) / 100, cy - s * 30 / 100 + i * s * 22 / 100, s * 140 / 100 - (i % 2) * s * 28 / 100, s * 12 / 100 > 2 ? s * 12 / 100 : 2, 2, c, 200);
}

// One icon centred at (cx, cy); `s` ~ half the icon width in pixels.
inline void icon(px *b, int cx, int cy, int s, nmweather::Condition c, bool day, px sky) {
	using namespace nmweather;
	const px white = rgb(255, 255, 255), shade = rgb(196, 214, 232), dark = rgb(120, 134, 154), darkShade = rgb(84, 96, 118);
	switch (c) {
		case COND_CLEAR:
			if (day) sun(b, cx, cy, s * 6 / 10, s >= 16); else moon(b, cx, cy, s * 7 / 10, sky);
			break;
		case COND_PARTLY:
			if (day) sun(b, cx - s * 25 / 100, cy - s * 22 / 100, s * 42 / 100, s >= 24); else moon(b, cx - s * 25 / 100, cy - s * 22 / 100, s * 48 / 100, sky);
			cloud(b, cx + s * 12 / 100, cy + s * 14 / 100, s * 80 / 100, white, shade);
			break;
		case COND_CLOUDY: cloud(b, cx, cy, s, day ? white : rgb(200, 210, 226), day ? shade : rgb(150, 166, 190)); break;
		case COND_FOG: cloud(b, cx, cy - s * 22 / 100, s * 80 / 100, rgb(230, 236, 242), rgb(190, 202, 214)); fogBars(b, cx, cy + s * 30 / 100, s, rgb(240, 244, 248)); break;
		case COND_RAIN: cloud(b, cx, cy - s * 14 / 100, s, dark, darkShade); rain(b, cx, cy - s * 14 / 100, s, s >= 24 ? 5 : 3, rgb(120, 190, 255)); break;
		case COND_STORM: cloud(b, cx, cy - s * 18 / 100, s, rgb(96, 104, 130), rgb(66, 72, 96)); bolt(b, cx, cy - s * 18 / 100, s); break;
		case COND_SNOW: cloud(b, cx, cy - s * 14 / 100, s, rgb(236, 242, 250), rgb(184, 200, 222)); snow(b, cx, cy - s * 14 / 100, s); break;
		default: roundRect(b, cx - s / 2, cy - s / 2, s, s, s / 4, rgb(220, 226, 236)); text(b, cx - 3 * (s >= 16 ? 2 : 1), cy - 7, "?", s >= 16 ? 2 : 1, rgb(90, 100, 120)); break;
	}
}

inline void sky(px *b, nmweather::Condition c, bool day) {
	const Sky s = skyFor(c, day);
	gradient(b, 0, H, s.top, s.bottom);
	if (!day) {
		uint32_t seed = 12345;
		for (int i = 0; i < 46; i++) {
			seed = seed * 1103515245u + 12345u;
			const int x = (seed >> 8) % W;
			seed = seed * 1103515245u + 12345u;
			const int y = (seed >> 8) % (H * 2 / 3);
			putA(b, x, y, rgb(255, 255, 255), 150 + (i % 3) * 40);
		}
	} else if (c == nmweather::COND_CLEAR || c == nmweather::COND_PARTLY) {
		// soft light bloom upper right
		for (int r = 90; r > 10; r -= 10)
			disc(b, 196, 52, r, rgb(255, 255, 255), 10);
	}
	// glossy horizon band
	rect(b, 0, H - 28, W, 28, rgb(255, 255, 255), 28);
}

// Glass panel
inline void glass(px *b, int x, int y, int w, int h, int r = 8) {
	roundRect(b, x, y, w, h, r, rgb(14, 50, 104), 84); // blue tint keeps white text readable on any sky
	roundRect(b, x, y, w, h, r, rgb(255, 255, 255), 40);
	roundGrad(b, x, y, w, h / 2, r, rgb(255, 255, 255), rgb(255, 255, 255), 28);
	// 1px light edge
	for (int i = r; i < w - r; i++) { putA(b, x + i, y, rgb(255, 255, 255), 150); putA(b, x + i, y + h - 1, rgb(255, 255, 255), 90); }
	for (int j = r; j < h - r; j++) { putA(b, x, y + j, rgb(255, 255, 255), 120); putA(b, x + w - 1, y + j, rgb(255, 255, 255), 90); }
}

// ======================================================================================================================
//  Layout
// ======================================================================================================================
struct Rect { int x, y, w, h; };
inline bool inside(const Rect &r, int px_, int py_) { return px_ >= r.x && py_ >= r.y && px_ < r.x + r.w && py_ < r.y + r.h; }

enum Button { BTN_NONE = 0, BTN_CITY, BTN_REFRESH, BTN_UNITS, BTN_AUTO, BTN_INTERVAL, BTN_NETWORK, BTN_BACK, BTN_RETRY,
			  KB_SPACE = 100, KB_BKSP, KB_SEARCH, KB_CANCEL, ROW_BASE = 200 };

struct Btn { Button id; Rect r; };

constexpr int MAIN_BTN_COUNT = 7;
inline void mainButtons(Btn out[MAIN_BTN_COUNT]) {
	out[0] = {BTN_CITY, {2, 120, 62, 26}};
	out[1] = {BTN_REFRESH, {66, 120, 62, 26}};
	out[2] = {BTN_UNITS, {130, 120, 62, 26}};
	out[3] = {BTN_AUTO, {194, 120, 60, 26}};
	out[4] = {BTN_INTERVAL, {2, 152, 62, 26}};
	out[5] = {BTN_NETWORK, {66, 152, 62, 26}};
	out[6] = {BTN_BACK, {130, 152, 124, 26}};
}

constexpr int KB_MAX_KEYS = 40;
struct Key { Button id; char ch; Rect r; const char *label; };
// QWERTY on the bottom screen: 4 rows of 24 px keys.
inline int keyboardKeys(Key out[KB_MAX_KEYS]) {
	static const char *rows[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
	static char labels[26][2];
	int n = 0;
	for (int r = 0; r < 3; r++) {
		const int len = (int)strlen(rows[r]);
		const int x0 = (W - len * 25) / 2 + (r == 2 ? -14 : 0);
		for (int i = 0; i < len; i++) {
			const char ch = rows[r][i];
			labels[ch - 'A'][0] = ch;
			labels[ch - 'A'][1] = 0;
			out[n++] = {(Button)ch, ch, {x0 + i * 25, 66 + r * 27, 24, 25}, labels[ch - 'A']};
		}
	}
	out[n++] = {KB_BKSP, 0, {(W - 7 * 25) / 2 - 14 + 7 * 25 + 1, 66 + 2 * 27, 36, 25}, "DEL"};
	out[n++] = {KB_CANCEL, 0, {6, 66 + 3 * 27 + 2, 56, 25}, "CANCEL"};
	out[n++] = {KB_SPACE, ' ', {66, 66 + 3 * 27 + 2, 120, 25}, "SPACE"};
	out[n++] = {KB_SEARCH, 0, {190, 66 + 3 * 27 + 2, 60, 25}, "SEARCH"};
	return n;
}

constexpr int RESULT_ROWS = 6;
inline Rect resultRow(int i) { return {6, 6 + i * 27, 244, 25}; }
inline Rect backButton() { return {6, 168, 76, 20}; }
inline Rect retryButton() { return {96, 168, 76, 20}; }

// D-pad navigation over a set of rectangles: from `sel`, the nearest rectangle in direction (dx, dy) (-1/0/1 each); sel when none.
inline int navigate(const Rect *r, int n, int sel, int dx, int dy) {
	if (sel < 0 || sel >= n)
		return n > 0 ? 0 : -1;
	const int cx = r[sel].x + r[sel].w / 2, cy = r[sel].y + r[sel].h / 2;
	int best = sel;
	long bestScore = 1L << 30;
	for (int i = 0; i < n; i++) {
		if (i == sel)
			continue;
		const int ox = r[i].x + r[i].w / 2 - cx, oy = r[i].y + r[i].h / 2 - cy;
		const int along = dx ? ox * dx : oy * dy;
		if (along <= 0)
			continue;
		const int across = dx ? (oy < 0 ? -oy : oy) : (ox < 0 ? -ox : ox);
		if (across > along * 2 + 8)
			continue; // mostly sideways: not "in that direction"
		const long score = (long)along + (long)across * 3;
		if (score < bestScore) {
			bestScore = score;
			best = i;
		}
	}
	return best;
}

// Which button is under (x, y)? `list` entries are checked in order.
inline Button hitMain(int x, int y) {
	Btn b[MAIN_BTN_COUNT];
	mainButtons(b);
	for (const Btn &k : b)
		if (inside(k.r, x, y))
			return k.id;
	return BTN_NONE;
}
inline Button hitKeyboard(int x, int y) {
	Key k[KB_MAX_KEYS];
	const int n = keyboardKeys(k);
	for (int i = 0; i < n; i++)
		if (inside(k[i].r, x, y))
			return k[i].id;
	return BTN_NONE;
}
inline int hitResults(int x, int y, int count) {
	for (int i = 0; i < count && i < RESULT_ROWS; i++)
		if (inside(resultRow(i), x, y))
			return i;
	return -1;
}

// ======================================================================================================================
//  Screens
// ======================================================================================================================
inline void glossButton(px *b, const Rect &r, const char *label, px c0, px c1, bool selected = false, px textColour = rgb(255, 255, 255)) {
	roundRect(b, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 7, selected ? rgb(255, 255, 255) : rgb(50, 100, 150), selected ? 255 : 160);
	roundGrad(b, r.x, r.y, r.w, r.h, 6, c0, c1);
	roundGrad(b, r.x + 2, r.y + 1, r.w - 4, r.h / 2 - 1, 5, rgb(255, 255, 255), rgb(255, 255, 255), 70); // gloss
	char t[24];
	fit(label, 1, r.w - 6, t, sizeof t);
	textC(b, r.x + r.w / 2, r.y + (r.h - 7) / 2 + 1, t, 1, textColour, false);
}

inline void bottomBackground(px *b) {
	gradient(b, 0, H, rgb(196, 228, 250), rgb(120, 180, 232));
	// gloss arc
	for (int i = 0; i < 6; i++)
		rect(b, 0, i * 3, W, 3, rgb(255, 255, 255), 52 - i * 8);
}

struct MainView {
	bool placeSet = false;
	bool haveData = false;
	nmweather::Forecast fc;
	char city[40] = "";
	char country[28] = "";
	bool fahrenheit = false;
	bool autoRefresh = true;
	int intervalMin = 60;
	int64_t now = 0;
	bool stale = false;
	const char *message = "";
	bool messageIsError = false;
	int selected = -1;	// keyboard-selected button (index into mainButtons), -1 none
};

inline const char *conditionText(nmweather::Condition c) {
	using namespace nmweather;
	switch (c) {
		case COND_CLEAR: return "CLEAR";
		case COND_PARTLY: return "PARTLY CLOUDY";
		case COND_CLOUDY: return "CLOUDY";
		case COND_FOG: return "FOG";
		case COND_RAIN: return "RAIN";
		case COND_STORM: return "THUNDERSTORM";
		case COND_SNOW: return "SNOW";
		default: return "UNKNOWN";
	}
}

inline const char *intervalText(int m) {
	switch (m) {
		case 30: return "EVERY 30M";
		case 180: return "EVERY 3H";
		case 360: return "EVERY 6H";
		default: return "EVERY 1H";
	}
}

inline void renderMainTop(px *b, const MainView &v) {
	using namespace nmweather;
	const bool day = v.haveData ? v.fc.isDay : true;
	const Condition c = v.haveData ? v.fc.now.cond : COND_CLOUDY;
	sky(b, c, day);
	const Sky s = skyFor(c, day);
	if (v.haveData)
		icon(b, 176, 78, 52, c, day, s.top);
	else
		icon(b, 176, 78, 52, COND_UNKNOWN, true, s.top);
	// left glass panel with the numbers
	glass(b, 8, 8, 128, 176, 10);
	char t[40];
	if (v.placeSet) {
		fit(v.city, 2, 116, t, sizeof t);
		textShadow(b, 14, 16, t, 2, rgb(255, 255, 255), rgb(30, 60, 100));
		if (v.country[0]) {
			fit(v.country, 1, 116, t, sizeof t);
			text(b, 14, 34, t, 1, rgb(240, 248, 255));
		}
	} else {
		textShadow(b, 14, 16, "NO CITY", 2, rgb(255, 255, 255), rgb(30, 60, 100));
		text(b, 14, 34, "TAP CITY TO SET", 1, rgb(240, 248, 255));
	}
	if (v.haveData) {
		tempText(t, sizeof t, v.fc.now.tempC10, v.fahrenheit);
		// "31C" -> big digits, then a degree sign and the unit
		char num[8], unit[2] = {t[strlen(t) - 1], 0};
		snprintf(num, sizeof num, "%.*s", (int)strlen(t) - 1, t);
		const int nw = textW(num, 5);
		textShadow(b, 14, 62, num, 5, rgb(255, 255, 255), rgb(30, 60, 100));
		text(b, 14 + nw + 4, 64, "\xB0", 2, rgb(255, 255, 255));
		text(b, 14 + nw + 4, 82, unit, 2, rgb(255, 255, 255));
		textShadow(b, 14, 112, conditionText(c), 1, rgb(255, 255, 255), rgb(30, 60, 100));
		const Day *today = nullptr;
		const int64_t ld = localDay(v.now, v.fc.utcOffset);
		for (int i = 0; i < v.fc.dayCount; i++)
			if (dayNumber(v.fc.days[i].date) == ld) { today = &v.fc.days[i]; break; }
		if (today) {
			char hi[12], lo[12], line[40];
			tempText(hi, sizeof hi, today->hiC10, v.fahrenheit);
			tempText(lo, sizeof lo, today->loC10, v.fahrenheit);
			snprintf(line, sizeof line, "H %s  L %s", hi, lo);
			text(b, 14, 128, line, 1, rgb(255, 255, 255));
			if (today->precip >= 0) {
				snprintf(line, sizeof line, "RAIN CHANCE %d%%", today->precip);
				text(b, 14, 142, line, 1, rgb(235, 245, 255));
			}
		}
		char age[24];
		ageText(age, sizeof age, v.now, v.fc.now.time);
		snprintf(t, sizeof t, "UPDATED %s", age);
		text(b, 14, 164, t, 1, v.stale ? rgb(255, 224, 150) : rgb(235, 245, 255));
	} else {
		textShadow(b, 14, 70, "--", 5, rgb(255, 255, 255), rgb(30, 60, 100));
		text(b, 14, 112, v.placeSet ? "NO DATA YET" : "CHOOSE A CITY", 1, rgb(255, 255, 255));
		text(b, 14, 128, v.placeSet ? "TAP REFRESH" : "TO SEE THE WEATHER", 1, rgb(235, 245, 255));
	}
}

inline void renderMainBottom(px *b, const MainView &v) {
	using namespace nmweather;
	bottomBackground(b);
	// forecast rows
	const int64_t ld = v.haveData ? localDay(v.now, v.fc.utcOffset) : 0;
	int row = 0;
	if (v.haveData) {
		for (int i = 0; i < v.fc.dayCount && row < 5; i++) {
			const Day &d = v.fc.days[i];
			const int64_t dn = dayNumber(d.date);
			if (dn != INT64_MIN && dn < ld)
				continue; // a day that has already passed (old cache)
			const int y = 4 + row * 22;
			roundGrad(b, 4, y, 248, 20, 6, rgb(255, 255, 255), rgb(222, 240, 252), 150);
			char label[16], hi[12], lo[12], both[24];
			dayLabel(label, sizeof label, d.date, ld);
			text(b, 10, y + 3, label, 2, rgb(24, 62, 104));
			icon(b, 122, y + 10, 8, d.cond, true, rgb(120, 180, 232));
			if (d.precip >= 0) {
				snprintf(both, sizeof both, "%d%%", d.precip);
				text(b, 138, y + 6, both, 1, rgb(60, 110, 170));
			}
			tempText(hi, sizeof hi, d.hiC10, v.fahrenheit);
			tempText(lo, sizeof lo, d.loC10, v.fahrenheit);
			hi[strlen(hi) - 1] = 0; // the unit is shown once, after the low
			snprintf(both, sizeof both, "%s/%s", hi, lo);
			textR(b, 249, y + 3, both, 2, rgb(24, 62, 104));
			row++;
		}
	}
	if (row == 0) {
		glass(b, 4, 4, 248, 104, 8);
		textC(b, 128, 40, v.haveData ? "NO FORECAST DAYS" : (v.placeSet ? "NO WEATHER DATA YET" : "NO CITY CHOSEN"), 1, rgb(24, 62, 104));
		textC(b, 128, 58, v.placeSet ? "TAP REFRESH (NEEDS WI-FI)" : "TAP CITY TO SEARCH FOR ONE", 1, rgb(24, 62, 104));
	}
	// status / message line
	if (v.message && v.message[0]) {
		char m[48];
		fit(v.message, 1, 244, m, sizeof m);
		text(b, 6, 108, m, 1, v.messageIsError ? rgb(176, 30, 30) : rgb(24, 62, 104));
	}
	// buttons
	Btn btns[MAIN_BTN_COUNT];
	mainButtons(btns);
	const px blue0 = rgb(110, 190, 250), blue1 = rgb(30, 110, 200), green0 = rgb(130, 220, 120), green1 = rgb(40, 150, 60), grey0 = rgb(190, 200, 214), grey1 = rgb(120, 134, 156);
	char unitLabel[16];
	snprintf(unitLabel, sizeof unitLabel, "UNITS %c", v.fahrenheit ? 'F' : 'C');
	char autoLabel[16];
	snprintf(autoLabel, sizeof autoLabel, "AUTO %s", v.autoRefresh ? "ON" : "OFF");
	for (int i = 0; i < MAIN_BTN_COUNT; i++) {
		const char *label = "";
		px c0 = blue0, c1 = blue1;
		switch (btns[i].id) {
			case BTN_CITY: label = "CITY"; break;
			case BTN_REFRESH: label = "REFRESH"; c0 = green0; c1 = green1; break;
			case BTN_UNITS: label = unitLabel; break;
			case BTN_AUTO: label = autoLabel; if (!v.autoRefresh) { c0 = grey0; c1 = grey1; } break;
			case BTN_INTERVAL: label = intervalText(v.intervalMin); if (!v.autoRefresh) { c0 = grey0; c1 = grey1; } break;
			case BTN_NETWORK: label = "NETWORK"; break;
			case BTN_BACK: label = "BACK"; c0 = rgb(250, 170, 120); c1 = rgb(200, 80, 40); break;
			default: break;
		}
		glossButton(b, btns[i].r, label, c0, c1, v.selected == i);
	}
}

// ---- city search -----------------------------------------------------------------------------------------------------
inline void renderKeyboardTop(px *b, const char *query, const char *hint, bool busy) {
	sky(b, nmweather::COND_PARTLY, true);
	glass(b, 12, 20, 232, 150, 10);
	textShadow(b, 22, 30, "CHANGE CITY", 2, rgb(255, 255, 255), rgb(30, 60, 100));
	text(b, 22, 54, "TYPE A CITY NAME, THEN SEARCH", 1, rgb(255, 255, 255));
	roundRect(b, 22, 76, 212, 28, 5, rgb(255, 255, 255), 230);
	char q[40];
	fit(query, 2, 196, q, sizeof q);
	text(b, 28, 83, q, 2, rgb(24, 62, 104));
	const int cx = 28 + textW(q, 2) + 3;
	rect(b, cx, 82, 2, 16, rgb(40, 110, 200));
	if (hint && hint[0]) {
		char h[64];
		fit(hint, 1, 208, h, sizeof h);
		text(b, 22, 116, h, 1, busy ? rgb(255, 255, 255) : rgb(255, 236, 170));
	}
	text(b, 22, 150, "A KEY  B DEL  START SEARCH", 1, rgb(235, 245, 255));
}

inline void renderKeyboardBottom(px *b, int sel) {
	bottomBackground(b);
	Key k[KB_MAX_KEYS];
	const int n = keyboardKeys(k);
	for (int i = 0; i < n; i++) {
		const bool special = k[i].ch == 0 || k[i].id == KB_SPACE;
		px c0 = special ? rgb(250, 190, 130) : rgb(255, 255, 255), c1 = special ? rgb(210, 110, 50) : rgb(190, 222, 246);
		if (k[i].id == KB_SEARCH) { c0 = rgb(130, 220, 120); c1 = rgb(40, 150, 60); }
		glossButton(b, k[i].r, k[i].label, c0, c1, sel == i, special ? rgb(255, 255, 255) : rgb(24, 62, 104));
	}
	text(b, 6, 8, "SEARCH", 2, rgb(24, 62, 104));
}

inline void renderResultsTop(px *b, const nmweather::PlaceList &l, int sel, const char *query) {
	sky(b, nmweather::COND_PARTLY, true);
	glass(b, 12, 20, 232, 150, 10);
	textShadow(b, 22, 30, "PICK A PLACE", 2, rgb(255, 255, 255), rgb(30, 60, 100));
	char t[64];
	snprintf(t, sizeof t, "RESULTS FOR %s", query);
	char f[64];
	fit(t, 1, 208, f, sizeof f);
	text(b, 22, 54, f, 1, rgb(255, 255, 255));
	if (sel >= 0 && sel < l.count) {
		const nmweather::Place &p = l.p[sel];
		fit(p.name, 2, 208, t, sizeof t);
		text(b, 22, 80, t, 2, rgb(255, 255, 255));
		snprintf(t, sizeof t, "%s%s%s", p.region, p.region[0] && p.country[0] ? ", " : "", p.country);
		fit(t, 1, 208, f, sizeof f);
		text(b, 22, 100, f, 1, rgb(255, 255, 255));
		snprintf(t, sizeof t, "LAT %.2f  LON %.2f", p.lat, p.lon);
		text(b, 22, 114, t, 1, rgb(235, 245, 255));
		if (p.timezone[0]) {
			fit(p.timezone, 1, 208, f, sizeof f);
			text(b, 22, 128, f, 1, rgb(235, 245, 255));
		}
	}
	text(b, 22, 150, "TAP A PLACE  OR  UP/DOWN + A", 1, rgb(235, 245, 255));
}

inline void renderResultsBottom(px *b, const nmweather::PlaceList &l, int sel) {
	bottomBackground(b);
	for (int i = 0; i < l.count && i < RESULT_ROWS; i++) {
		const Rect r = resultRow(i);
		const bool on = i == sel;
		roundGrad(b, r.x, r.y, r.w, r.h, 6, on ? rgb(255, 250, 200) : rgb(255, 255, 255), on ? rgb(255, 214, 120) : rgb(222, 240, 252), on ? 255 : 170);
		char name[32], sub[48], f[48];
		fit(l.p[i].name, 2, r.w - 12, name, sizeof name);
		text(b, r.x + 6, r.y + 2, name, 2, rgb(24, 62, 104));
		snprintf(sub, sizeof sub, "%s%s%s", l.p[i].region, l.p[i].region[0] && l.p[i].country[0] ? ", " : "", l.p[i].country);
		fit(sub, 1, r.w - 12, f, sizeof f);
		text(b, r.x + 6, r.y + 17, f, 1, rgb(60, 110, 170));
	}
	if (l.count == 0)
		textC(b, 128, 70, "NO PLACE FOUND", 2, rgb(24, 62, 104));
	glossButton(b, backButton(), "BACK", rgb(250, 170, 120), rgb(200, 80, 40));
}

// ---- busy dialog (top screen) and network test ---------------------------------------------------------------------------
inline void renderBusy(px *b, const char *title, const char *status, int frame, const char *footer) {
	sky(b, nmweather::COND_CLOUDY, true);
	icon(b, 128, 64, 44, nmweather::COND_PARTLY, true, rgb(104, 134, 168));
	glass(b, 20, 110, 216, 72, 10);
	textC(b, 128, 118, title, 2, rgb(255, 255, 255), true);
	char s[48];
	fit(status, 1, 200, s, sizeof s);
	textC(b, 128, 142, s, 1, rgb(255, 255, 255));
	const int dots = (frame / 15) % 4;
	for (int i = 0; i < 3; i++)
		disc(b, 112 + i * 16, 160, 3, i < dots ? rgb(255, 255, 255) : rgb(160, 190, 220));
	if (footer && footer[0])
		textC(b, 128, 170, footer, 1, rgb(235, 245, 255));
}

struct TestLine { char name[20]; char result[44]; int state; }; // state: 0 pending, 1 ok, 2 fail, 3 running
constexpr int TEST_MAX = 8;
inline void renderNetTestTop(px *b, const TestLine *lines, int count, const char *summary, bool summaryOk) {
	sky(b, nmweather::COND_CLOUDY, true);
	glass(b, 6, 6, 244, 180, 10);
	textShadow(b, 14, 12, "NETWORK TEST", 2, rgb(255, 255, 255), rgb(30, 60, 100));
	for (int i = 0; i < count; i++) {
		const int y = 36 + i * 17;
		const TestLine &l = lines[i];
		const px dot = l.state == 1 ? rgb(60, 200, 70) : l.state == 2 ? rgb(230, 60, 50) : l.state == 3 ? rgb(250, 200, 60) : rgb(170, 180, 196);
		disc(b, 18, y + 3, 4, dot);
		text(b, 28, y, l.name, 1, rgb(255, 255, 255));
		char f[48];
		fit(l.result, 1, 244 - 98, f, sizeof f);
		text(b, 98, y, f, 1, l.state == 2 ? rgb(255, 214, 200) : rgb(235, 245, 255));
	}
	if (summary && summary[0]) {
		char f[64];
		fit(summary, 1, 232, f, sizeof f);
		text(b, 14, 36 + count * 17 + 4, f, 1, summaryOk ? rgb(200, 255, 200) : rgb(255, 230, 160));
	}
}
inline void renderBusyBottom(px *b) {
	bottomBackground(b);
	textC(b, 128, 70, "PLEASE WAIT", 2, rgb(24, 62, 104));
	textC(b, 128, 96, "THE NETWORK CAN BE SLOW", 1, rgb(24, 62, 104));
	glossButton(b, backButton(), "CANCEL", rgb(250, 170, 120), rgb(200, 80, 40));
}
inline void renderNetTestBottom(px *b) {
	bottomBackground(b);
	textC(b, 128, 60, "CHECKS YOUR WI-FI, DNS,", 1, rgb(24, 62, 104));
	textC(b, 128, 74, "SECURE CONNECTION AND CLOCK", 1, rgb(24, 62, 104));
	glossButton(b, backButton(), "BACK", rgb(250, 170, 120), rgb(200, 80, 40));
	glossButton(b, retryButton(), "RETRY", rgb(130, 220, 120), rgb(40, 150, 60));
}

} // namespace wd
