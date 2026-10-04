#include "filters.h"

#include <malloc.h>

namespace fx {
namespace {

Effect cur = NORMAL;
u16 *lut = nullptr; // 32768 entries: RGB555 (without bit 15) -> RGB555 (without bit 15)

inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline int luma(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }

void colour(Effect e, u8 &r, u8 &g, u8 &b) {
	switch (e) {
		case MONO: {
			const int l = luma(r, g, b);
			r = g = b = (u8)l;
			break;
		}
		case SEPIA: {
			const int l = luma(r, g, b);
			r = (u8)clamp8(((l * 282) >> 8) + 16);
			g = (u8)clamp8(((l * 242) >> 8) + 8);
			b = (u8)clamp8((l * 192) >> 8);
			break;
		}
		case NEGATIVE:
			r = 255 - r;
			g = 255 - g;
			b = 255 - b;
			break;
		case COOL:
			r = (u8)((r * 224) >> 8);
			b = (u8)clamp8(((b * 288) >> 8) + 8);
			break;
		case WARM:
			r = (u8)clamp8(((r * 288) >> 8) + 8);
			b = (u8)((b * 224) >> 8);
			break;
		case POSTERIZE: // four levels per channel: 0, 85, 170, 255
			r = (u8)((((r * 3) + 128) >> 8) * 85);
			g = (u8)((((g * 3) + 128) >> 8) * 85);
			b = (u8)((((b * 3) + 128) >> 8) * 85);
			break;
		case CONTRAST:
			r = (u8)clamp8((((int)r - 128) * 320 >> 8) + 128);
			g = (u8)clamp8((((int)g - 128) * 320 >> 8) + 128);
			b = (u8)clamp8((((int)b - 128) * 320 >> 8) + 128);
			break;
		default:
			break;
	}
}

inline bool usesLut(Effect e) { return e != NORMAL && e != MIRROR; }

} // namespace

const char *name(Effect e) {
	static const char *const names[COUNT] = {"NORMAL", "MONO", "SEPIA", "NEGATIVE", "COOL", "WARM", "POSTERIZE", "HIGH CONTRAST", "MIRROR"};
	return (e >= 0 && e < COUNT) ? names[e] : "NORMAL";
}

const char *statusLabel(Effect e) {
	static const char *const labels[COUNT] = {nullptr, "Effect: MONO", "Effect: SEPIA", "Effect: NEGATIVE", "Effect: COOL", "Effect: WARM", "Effect: POSTERIZE", "Effect: CONTRAST", "Effect: MIRROR"};
	return (e > 0 && e < COUNT) ? labels[e] : nullptr;
}

bool videoSupported(Effect e) { return e == NORMAL || e == MONO || e == SEPIA || e == NEGATIVE; }

Effect current() { return cur; }

bool set(Effect e) {
	if (e < 0 || e >= COUNT)
		e = NORMAL;
	if (!usesLut(e)) {
		free(lut);
		lut = nullptr;
		cur = e;
		return true;
	}
	if (!lut)
		lut = (u16 *)malloc(32768 * sizeof(u16));
	if (!lut) {
		cur = NORMAL;
		return false;
	}
	for (int p = 0; p < 32768; p++) {
		const int r5 = p & 31, g5 = (p >> 5) & 31, b5 = (p >> 10) & 31;
		u8 r = (u8)((r5 << 3) | (r5 >> 2)), g = (u8)((g5 << 3) | (g5 >> 2)), b = (u8)((b5 << 3) | (b5 >> 2));
		colour(e, r, g, b);
		lut[p] = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
	}
	cur = e;
	return true;
}

Effect step(Effect from, int dir, bool videoOnly) {
	int e = from;
	for (int i = 0; i < COUNT; i++) {
		e = (e + (dir < 0 ? COUNT - 1 : 1)) % COUNT;
		if (!videoOnly || videoSupported((Effect)e))
			return (Effect)e;
	}
	return from;
}

void applyFrame(u16 *frame) {
	if (cur == NORMAL || !frame)
		return;
	if (cur == MIRROR) {
		for (int y = 0; y < 192; y++) {
			u16 *row = frame + y * 256;
			for (int x = 0; x < 128; x++)
				row[255 - x] = row[x];
		}
		return;
	}
	if (!lut)
		return;
	const u16 *t = lut;
	for (int i = 0; i < 256 * 192; i++) {
		const u16 p = frame[i];
		frame[i] = (u16)(t[p & 0x7FFF] | (p & 0x8000));
	}
}

void applyRgb8(u8 &r, u8 &g, u8 &b) {
	if (cur == NORMAL || cur == MIRROR)
		return;
	colour(cur, r, g, b);
}

} // namespace fx
