#include "gameArt.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "ThemeConfig.h"
#include "ThemeTextures.h"
#include "homeWidgets.h"
#include "common/nmfont.h"
#include "common/lodepng.h"
#include "common/tonccpy.h"
#include "common/twlmenusettings.h"

extern u16 *colorTable;
extern uint photoWidth, photoHeight;

namespace gameArt {
namespace {

constexpr int PX = PANEL_W * PANEL_H;
constexpr int CACHE_N = 4;

struct Cached {
	u32 key = 0;
	bool used = false;
	u32 stamp = 0;
	u16 pixels[PX];
};

Cached cache[CACHE_N];
u32 artClock = 0;
bool shown = false;
u32 shownKey = 0;

u32 hashKey(const std::string &s) {
	u32 h = 2166136261u;
	for (unsigned char c : s)
		h = (h ^ c) * 16777619u;
	return h ? h : 1;
}

inline u16 rgb(int r, int g, int b) { return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | BIT(15)); }

// Panel background tone by key: a handful of calm colours
u16 toneFor(u32 key) {
	static const u16 tones[6] = {rgb(52, 84, 140), rgb(58, 120, 110), rgb(120, 78, 64), rgb(96, 70, 130), rgb(70, 110, 70), rgb(132, 100, 56)};
	return tones[key % 6];
}

void fillPanel(u16 *p, u16 bg) {
	for (int i = 0; i < PX; i++)
		p[i] = bg;
	// 1 px light frame
	const u16 edge = rgb(235, 240, 248);
	for (int x = 0; x < PANEL_W; x++)
		p[x] = p[(PANEL_H - 1) * PANEL_W + x] = edge;
	for (int y = 0; y < PANEL_H; y++)
		p[y * PANEL_W] = p[y * PANEL_W + PANEL_W - 1] = edge;
}

// 5x7 glyphs for the system labels only
const u8 *glyph(char c) {
	static const u8 A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
	static const u8 B[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
	static const u8 C[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
	static const u8 D[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
	static const u8 E[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
	static const u8 G[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
	static const u8 M[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
	static const u8 N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
	static const u8 S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
	switch (c) {
		case 'A': return A;
		case 'B': return B;
		case 'C': return C;
		case 'D': return D;
		case 'E': return E;
		case 'G': return G;
		case 'M': return M;
		case 'N': return N;
		case 'S': return S;
		default: return nullptr;
	}
}

void drawLabel(u16 *p, const char *label, int scale, int cy, u16 colour) {
	const int len = (int)strlen(label);
	if (len == 0)
		return;
	const int w = len * 6 * scale - scale;
	int x0 = (PANEL_W - w) / 2;
	for (int i = 0; i < len; i++) {
		const u8 *g = glyph(label[i]);
		if (g) {
			for (int row = 0; row < 7; row++) {
				for (int col = 0; col < 5; col++) {
					if (g[row] & (0x10 >> col)) {
						for (int dy = 0; dy < scale; dy++)
							for (int dx = 0; dx < scale; dx++) {
								const int x = x0 + col * scale + dx, y = cy + row * scale + dy;
								if (x > 0 && x < PANEL_W - 1 && y > 0 && y < PANEL_H - 1)
									p[y * PANEL_W + x] = colour;
							}
					}
				}
			}
		}
		x0 += 6 * scale;
	}
}

Cached *slotFor(u32 key, bool &fresh) {
	Cached *oldest = &cache[0];
	for (Cached &c : cache) {
		if (c.used && c.key == key) {
			c.stamp = ++artClock;
			fresh = false;
			return &c;
		}
		if (!c.used || c.stamp < oldest->stamp)
			oldest = &c;
	}
	oldest->used = false;
	fresh = true;
	return oldest;
}

// ---- blit / restore ---------------------------------------------------------------------------------

u16 mapColour(u16 c) { return colorTable ? (u16)(colorTable[c % 0x8000] | BIT(15)) : c; }

void blit(const u16 *panel) {
	u16 *b1 = ThemeTextures::beginBgSubModify();
	u16 *b2 = boxArtColorDeband ? ThemeTextures::bgSubBuffer2() : nullptr;
	for (int y = 0; y < PANEL_H; y++) {
		u16 *d1 = b1 + (PANEL_Y + y) * 256 + PANEL_X;
		u16 *d2 = b2 ? b2 + (PANEL_Y + y) * 256 + PANEL_X : nullptr;
		for (int x = 0; x < PANEL_W; x++) {
			const u16 c = mapColour(panel[y * PANEL_W + x]);
			d1[x] = c;
			if (d2)
				d2[x] = c;
		}
	}
	ThemeTextures::commitBgSubModify();
}

} // namespace

bool enabled() {
	return ms().theme == TWLSettings::EThemeDSi && !ms().macroMode && ms().showPhoto && tc().renderPhoto();
}

bool visible() { return shown; }

void forget() { shown = false; }

void clear() {
	if (!shown)
		return;
	shown = false;
	u16 *b1 = ThemeTextures::beginBgSubModify();
	u16 *b2 = boxArtColorDeband ? ThemeTextures::bgSubBuffer2() : nullptr;
	const u16 *p1 = ThemeTextures::photoBuffer();
	const u16 *p2 = boxArtColorDeband ? ThemeTextures::photoBuffer2() : nullptr;
	const int photoX = 24 + (208 - (int)photoWidth) / 2;
	const int photoY = 24 + (156 - (int)photoHeight) / 2;
	for (int y = 0; y < PANEL_H; y++) {
		const int sy = PANEL_Y + y;
		for (int x = 0; x < PANEL_W; x++) {
			const int sx = PANEL_X + x;
			const int px = sx - photoX, py = sy - photoY;
			const bool inPhoto = p1 && px >= 0 && py >= 0 && px < (int)photoWidth && py < (int)photoHeight;
			b1[sy * 256 + sx] = inPhoto ? p1[py * photoWidth + px] : 0x8000; // outside the picture the frame is black
			if (b2)
				b2[sy * 256 + sx] = inPhoto && p2 ? p2[py * photoWidth + px] : 0x8000;
		}
	}
	ThemeTextures::commitBgSubModify();
	homeWidgets::artCleared(); // the left lane is free again: the Play Stats card goes back
}

static char cap1[12] = "", cap2[12] = "";

void setCaption(const char *line1, const char *line2) {
	snprintf(cap1, sizeof(cap1), "%.10s", line1 ? line1 : "");
	snprintf(cap2, sizeof(cap2), "%.10s", line2 ? line2 : "");
}

static bool present(Cached *c, u32 key) {
	const u32 shownAs = key ^ hashKey(std::string(cap1) + "|" + cap2);
	if (shown && shownKey == shownAs)
		return true; // already on screen
	if (cap1[0] || cap2[0]) {
		static u16 withCaption[PX];
		memcpy(withCaption, c->pixels, sizeof(withCaption));
		// dark strip over the bottom of the panel, then the text
		for (int y = PANEL_H - 19; y < PANEL_H - 1; y++)
			for (int x = 1; x < PANEL_W - 1; x++) {
				const u16 v = withCaption[y * PANEL_W + x];
				withCaption[y * PANEL_W + x] = (u16)((((v & 31) >> 1) | ((((v >> 5) & 31) >> 1) << 5) | ((((v >> 10) & 31) >> 1) << 10)) | BIT(15));
			}
		if (cap1[0])
			nmfont::drawCentred(withCaption, PANEL_W, PANEL_H, PANEL_H - 17, cap1, 1, rgb(255, 255, 255));
		if (cap2[0])
			nmfont::drawCentred(withCaption, PANEL_W, PANEL_H, PANEL_H - 9, cap2, 1, rgb(190, 215, 240));
		blit(withCaption);
	} else {
		blit(c->pixels);
	}
	shown = true;
	shownKey = shownAs;
	return true;
}

bool showBoxArtFile(const std::string &key, const char *pngPath) {
	const u32 k = hashKey(key);
	bool fresh;
	Cached *c = slotFor(k, fresh);
	if (fresh) {
		// Check the size from the PNG header before decoding: a decode needs w*h*4 bytes of heap
		FILE *f = fopen(pngPath, "rb");
		if (!f)
			return false;
		u8 head[24] = {0};
		const size_t got = fread(head, 1, 24, f);
		fclose(f);
		if (got < 24 || memcmp(head, "\x89PNG\r\n\x1a\n", 8) != 0)
			return false;
		const u32 w = ((u32)head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
		const u32 h = ((u32)head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
		if (w == 0 || h == 0 || w > 256 || h > 192)
			return false;
		std::vector<unsigned char> img;
		unsigned iw = 0, ih = 0;
		if (lodepng::decode(img, iw, ih, pngPath) != 0 || img.size() < (size_t)iw * ih * 4 || iw == 0 || ih == 0)
			return false;

		fillPanel(c->pixels, toneFor(k));
		// fit into the inside of the frame, aspect kept, box-averaged
		const int aw = PANEL_W - 4, ah = PANEL_H - 4;
		int dw = aw, dh = (int)(((u64)ih * aw) / iw);
		if (dh > ah) {
			dh = ah;
			dw = (int)(((u64)iw * ah) / ih);
		}
		if (dw < 1) dw = 1;
		if (dh < 1) dh = 1;
		const int ox = (PANEL_W - dw) / 2, oy = (PANEL_H - dh) / 2;
		for (int y = 0; y < dh; y++) {
			const u32 y0 = (u32)(((u64)y * ih) / dh);
			u32 y1 = (u32)(((u64)(y + 1) * ih) / dh);
			if (y1 <= y0) y1 = y0 + 1;
			for (int x = 0; x < dw; x++) {
				const u32 x0 = (u32)(((u64)x * iw) / dw);
				u32 x1 = (u32)(((u64)(x + 1) * iw) / dw);
				if (x1 <= x0) x1 = x0 + 1;
				u32 sum[4] = {0, 0, 0, 0};
				for (u32 sy = y0; sy < y1 && sy < ih; sy++) {
					const unsigned char *px = &img[((size_t)sy * iw + x0) * 4];
					for (u32 sx = x0; sx < x1 && sx < iw; sx++, px += 4) {
						sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2]; sum[3] += px[3];
					}
				}
				const u32 n = (y1 - y0) * (x1 - x0);
				const u32 a = sum[3] / n;
				u16 *d = &c->pixels[(oy + y) * PANEL_W + ox + x];
				if (a >= 250) {
					*d = rgb(sum[0] / n, sum[1] / n, sum[2] / n);
				} else if (a > 0) {
					const u16 bg = *d;
					const int br = (bg & 31) << 3, bgc = ((bg >> 5) & 31) << 3, bb = ((bg >> 10) & 31) << 3;
					*d = rgb((sum[0] / n * a + br * (255 - a)) / 255, (sum[1] / n * a + bgc * (255 - a)) / 255, (sum[2] / n * a + bb * (255 - a)) / 255);
				}
			}
		}
		c->key = k;
		c->used = true;
		c->stamp = ++artClock;
	}
	return present(c, k);
}

bool showIcon(const std::string &key, const char *romPath, const char *label) {
	const u32 k = hashKey(key);
	bool fresh;
	Cached *c = slotFor(k, fresh);
	if (fresh) {
		FILE *f = fopen(romPath, "rb");
		if (!f)
			return false;
		u32 bannerOff = 0;
		u8 bnr[0x240];
		bool ok = fseek(f, 0x68, SEEK_SET) == 0 && fread(&bannerOff, 4, 1, f) == 1 && bannerOff >= 0x200 && bannerOff < 0x10000000 &&
				  fseek(f, bannerOff + 0x20, SEEK_SET) == 0 && fread(bnr, 1, 0x220, f) == 0x220;
		fclose(f);
		if (!ok)
			return false;
		const u8 *tiles = bnr;           // 32x32, 4 bpp, 4x4 tiles of 8x8
		const u16 *pal = (const u16 *)(bnr + 0x200); // 16 colours
		fillPanel(c->pixels, toneFor(k));
		// 32x32 icon enlarged 2x (64x64 would touch the frame: use 56x56 area -> 1.75x via nearest)
		const int size = 56;
		const int ox = (PANEL_W - size) / 2, oy = 10;
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				const int sx = x * 32 / size, sy = y * 32 / size;
				const int tile = (sy / 8) * 4 + (sx / 8);
				const int within = (sy % 8) * 8 + (sx % 8);
				const u8 byte = tiles[tile * 32 + within / 2];
				const int idx = (within & 1) ? (byte >> 4) : (byte & 15);
				if (idx == 0)
					continue; // transparent
				const u16 col = pal[idx];
				c->pixels[(oy + y) * PANEL_W + ox + x] = (u16)((col & 0x7FFF) | BIT(15));
			}
		}
		if (label && *label)
			drawLabel(c->pixels, label, 2, oy + size + 8, rgb(255, 255, 255));
		c->key = k;
		c->used = true;
		c->stamp = ++artClock;
	}
	return present(c, k);
}

bool showPlaceholder(const std::string &key, const char *label) {
	const u32 k = hashKey(key);
	bool fresh;
	Cached *c = slotFor(k, fresh);
	if (fresh) {
		fillPanel(c->pixels, toneFor(k));
		const int len = label ? (int)strlen(label) : 0;
		const int scale = len > 3 ? 2 : 3;
		if (len > 0)
			drawLabel(c->pixels, label, scale, (PANEL_H - 7 * scale) / 2, rgb(255, 255, 255));
		c->key = k;
		c->used = true;
		c->stamp = ++artClock;
	}
	return present(c, k);
}

} // namespace gameArt
