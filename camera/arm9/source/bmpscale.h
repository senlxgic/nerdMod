/*
	BMP header parsing and aspect-preserving scaling for the Photos app. Pure maths (no I/O), host-tested.
	Accepts uncompressed 24-bit and 32-bit BMPs (BI_RGB, and BI_BITFIELDS 32-bit with the usual BGRA masks),
	bottom-up or top-down. Anything else is rejected, never guessed at.
*/
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace bmpscale {

constexpr uint32_t MAX_DIM = 8192;			  // either side
constexpr uint32_t MAX_PIXELS = 16u << 20;	  // 16 MP: only rows are read, so this is a sanity bound, not a RAM bound

struct Info {
	bool ok = false;
	int w = 0, h = 0;		// h is positive
	int bpp = 0;			// 24 or 32
	bool topDown = false;
	uint32_t dataOffset = 0;
	uint32_t rowBytes = 0;	// padded to 4
};

inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// `hdr` holds at least the first `n` bytes of the file (54 needed); fileSize is the real size on disk.
inline Info parse(const uint8_t *hdr, size_t n, uint32_t fileSize) {
	Info r;
	if (n < 54 || hdr[0] != 'B' || hdr[1] != 'M')
		return r;
	const uint32_t dataOff = le32(hdr + 10);
	const uint32_t dibSize = le32(hdr + 14);
	if (dibSize < 40)
		return r; // OS/2 core headers are not supported
	const int32_t w = (int32_t)le32(hdr + 18);
	const int32_t hRaw = (int32_t)le32(hdr + 22);
	const uint16_t planes = le16(hdr + 26);
	const uint16_t bpp = le16(hdr + 28);
	const uint32_t compression = le32(hdr + 30);
	if (planes != 1 || (bpp != 24 && bpp != 32))
		return r;
	if (compression != 0 && !(compression == 3 && bpp == 32))
		return r;
	if (w <= 0 || hRaw == 0 || hRaw == INT32_MIN)
		return r;
	const int32_t h = hRaw < 0 ? -hRaw : hRaw;
	if ((uint32_t)w > MAX_DIM || (uint32_t)h > MAX_DIM || (uint64_t)w * (uint64_t)h > MAX_PIXELS)
		return r;
	if (compression == 3) {
		// BI_BITFIELDS: only the standard BGRA layout (masks follow the 40-byte header)
		if (n < 66 || le32(hdr + 54) != 0x00FF0000u || le32(hdr + 58) != 0x0000FF00u || le32(hdr + 62) != 0x000000FFu)
			return r;
	}
	const uint32_t rowBytes = ((uint32_t)w * (bpp / 8) + 3u) & ~3u;
	if (dataOff < 14 + dibSize || dataOff > fileSize)
		return r;
	if ((uint64_t)dataOff + (uint64_t)rowBytes * (uint64_t)h > (uint64_t)fileSize)
		return r; // truncated pixel data
	r.ok = true;
	r.w = w;
	r.h = h;
	r.bpp = bpp;
	r.topDown = hRaw < 0;
	r.dataOffset = dataOff;
	r.rowBytes = rowBytes;
	return r;
}

// File offset of picture row `y` (0 = top row of the picture).
inline uint64_t rowOffset(const Info &i, int y) {
	const int fileRow = i.topDown ? y : (i.h - 1 - y);
	return (uint64_t)i.dataOffset + (uint64_t)fileRow * i.rowBytes;
}

// Largest size that fits maxW x maxH with the source aspect ratio (never zero, never larger than the box).
inline void fit(int srcW, int srcH, int maxW, int maxH, int &dw, int &dh) {
	if (srcW <= 0 || srcH <= 0 || maxW <= 0 || maxH <= 0) {
		dw = dh = 0;
		return;
	}
	if ((int64_t)srcW * maxH >= (int64_t)srcH * maxW) {
		dw = maxW;
		dh = (int)(((int64_t)srcH * maxW) / srcW);
	} else {
		dh = maxH;
		dw = (int)(((int64_t)srcW * maxH) / srcH);
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	if (dw > maxW) dw = maxW;
	if (dh > maxH) dh = maxH;
}

// Nearest-neighbour source coordinate for destination index d of dstN (always inside [0, srcN)).
inline int srcCoord(int d, int dstN, int srcN) {
	const int64_t s = ((int64_t)(2 * d + 1) * srcN) / (2 * (int64_t)dstN);
	return s < 0 ? 0 : (s >= srcN ? srcN - 1 : (int)s);
}

inline uint16_t rgb555(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000); }

} // namespace bmpscale
