/*
	Frame encodings of the NERDVID container (Phase 2D). No dependencies, so the host tests can include it.

	  F_RGB555       256x192, 2 bytes per pixel (98304 bytes)  - the original format, lossless camera data
	  F_RGB332       256x192, 1 byte per pixel (49152 bytes)   - bits 7-5 R, 4-2 G, 1-0 B
	  F_RGB332_HALF  128x96,  1 byte per pixel (12288 bytes)   - box-filtered, shown at 2x by the player

	Every payload is a multiple of 512 bytes, so a slot (512 header bytes + payload) stays sector aligned.
	decode() always produces the 256x192 RGB555 picture (bit 15 set) the screen and the thumbnails use.
*/
#pragma once

#include <stdint.h>

namespace vfmt {

enum Quality { Q_HIGH = 0, Q_BALANCED = 1, Q_SMALL = 2 };
constexpr int QUALITY_COUNT = 3;
constexpr int DEFAULT_QUALITY = Q_BALANCED;

constexpr uint16_t F_RGB555 = 1;
constexpr uint16_t F_RGB332 = 2;
constexpr uint16_t F_RGB332_HALF = 3;

constexpr uint32_t SCREEN_W = 256, SCREEN_H = 192;

inline int sanitizeQuality(int q) { return (q >= 0 && q < QUALITY_COUNT) ? q : DEFAULT_QUALITY; }
inline const char *qualityName(int q) {
	switch (q) {
		case Q_HIGH: return "HIGH";
		case Q_SMALL: return "SMALL";
		default: return "BALANCED";
	}
}
inline const char *qualityHint(int q) {
	switch (q) {
		case Q_HIGH: return "Full colour, 98 KB/frame";
		case Q_SMALL: return "Half size, 12 KB/frame";
		default: return "256 colours, 49 KB/frame";
	}
}
inline uint16_t formatForQuality(int q) { return q == Q_HIGH ? F_RGB555 : q == Q_SMALL ? F_RGB332_HALF : F_RGB332; }
constexpr bool known(uint16_t f) { return f == F_RGB555 || f == F_RGB332 || f == F_RGB332_HALF; }
constexpr uint32_t widthOf(uint16_t f) { return f == F_RGB332_HALF ? 128 : 256; }
constexpr uint32_t heightOf(uint16_t f) { return f == F_RGB332_HALF ? 96 : 192; }
constexpr uint32_t bytesPerPixel(uint16_t f) { return f == F_RGB555 ? 2 : 1; }
constexpr uint32_t frameBytes(uint16_t f) { return known(f) ? widthOf(f) * heightOf(f) * bytesPerPixel(f) : 0; }
inline const char *formatTag(uint16_t f) { return f == F_RGB555 ? "RGB555" : f == F_RGB332 ? "RGB332" : f == F_RGB332_HALF ? "RGB332-HALF" : "?"; }

// RGB555 (bits 0-4 R, 5-9 G, 10-14 B) <-> RGB332
inline uint8_t toRgb332(uint16_t c) { return (uint8_t)((((c >> 2) & 7) << 5) | (((c >> 7) & 7) << 2) | ((c >> 13) & 3)); }
inline uint16_t fromRgb332(uint8_t v) {
	const unsigned r3 = v >> 5, g3 = (v >> 2) & 7, b2 = v & 3;
	const unsigned r5 = (r3 << 2) | (r3 >> 1), g5 = (g3 << 2) | (g3 >> 1), b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
	return (uint16_t)(0x8000u | r5 | (g5 << 5) | (b5 << 10));
}

// src: 256x192 RGB555. dst: frameBytes(f) bytes.
inline void encode(uint16_t f, const uint16_t *src, uint8_t *dst) {
	if (f == F_RGB555) {
		uint16_t *d = (uint16_t *)dst;
		for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; i++)
			d[i] = src[i];
	} else if (f == F_RGB332) {
		for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; i++)
			dst[i] = toRgb332(src[i]);
	} else if (f == F_RGB332_HALF) {
		for (uint32_t y = 0; y < 96; y++) {
			const uint16_t *a = src + (y * 2) * SCREEN_W, *b = a + SCREEN_W;
			for (uint32_t x = 0; x < 128; x++) {
				const uint16_t p0 = a[2 * x], p1 = a[2 * x + 1], p2 = b[2 * x], p3 = b[2 * x + 1];
				const uint32_t r = ((p0 & 31) + (p1 & 31) + (p2 & 31) + (p3 & 31)) >> 2;
				const uint32_t g = (((p0 >> 5) & 31) + ((p1 >> 5) & 31) + ((p2 >> 5) & 31) + ((p3 >> 5) & 31)) >> 2;
				const uint32_t bl = (((p0 >> 10) & 31) + ((p1 >> 10) & 31) + ((p2 >> 10) & 31) + ((p3 >> 10) & 31)) >> 2;
				dst[y * 128 + x] = (uint8_t)(((r >> 2) << 5) | ((g >> 2) << 2) | (bl >> 3));
			}
		}
	}
}

// src: frameBytes(f) bytes. dst: 256x192 RGB555.
inline void decode(uint16_t f, const uint8_t *src, uint16_t *dst) {
	static uint16_t lut[256];
	static bool ready = false;
	if (!ready) {
		for (int i = 0; i < 256; i++)
			lut[i] = fromRgb332((uint8_t)i);
		ready = true;
	}
	if (f == F_RGB555) {
		const uint16_t *s = (const uint16_t *)src;
		for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; i++)
			dst[i] = s[i];
	} else if (f == F_RGB332) {
		for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; i++)
			dst[i] = lut[src[i]];
	} else if (f == F_RGB332_HALF) {
		for (uint32_t y = 0; y < 96; y++) {
			uint16_t *r0 = dst + (y * 2) * SCREEN_W, *r1 = r0 + SCREEN_W;
			const uint8_t *s = src + y * 128;
			for (uint32_t x = 0; x < 128; x++) {
				const uint16_t c = lut[s[x]];
				r0[2 * x] = r0[2 * x + 1] = r1[2 * x] = r1[2 * x + 1] = c;
			}
		}
	}
}

} // namespace vfmt
