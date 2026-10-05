// nerdMod Music: MPEG audio frame header parsing (layer III) for duration estimates and seeking. Decoding is minimp3.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nmid3.h"

namespace nmmp3 {

struct Frame {
	bool ok = false;
	int version = 0;	  // 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5
	int bitrateKbps = 0;
	int rate = 0;
	int channels = 0;
	int samples = 0;	  // PCM samples per channel in this frame
	int bytes = 0;
};

inline Frame header(const uint8_t *h) {
	static const int BR1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1};
	static const int BR2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};
	static const int SR1[4] = {44100, 48000, 32000, 0};
	Frame f;
	if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
		return f;
	const int vbits = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
	if (vbits == 1 || layer != 1) // reserved version / not layer III
		return f;
	const int bi = h[2] >> 4, si = (h[2] >> 2) & 3;
	if (bi == 0 || bi == 15 || si == 3)
		return f; // free format and invalid values are not supported
	f.version = vbits == 3 ? 1 : (vbits == 2 ? 2 : 25);
	f.bitrateKbps = f.version == 1 ? BR1[bi] : BR2[bi];
	f.rate = f.version == 1 ? SR1[si] : (SR1[si] >> (f.version == 2 ? 1 : 2));
	f.channels = ((h[3] >> 6) == 3) ? 1 : 2;
	f.samples = f.version == 1 ? 1152 : 576;
	f.bytes = (f.version == 1 ? 144000 : 72000) * f.bitrateKbps / f.rate + ((h[2] >> 1) & 1);
	f.ok = f.bytes > 4;
	return f;
}

// Offset of the first real audio frame (skips ID3v2, then requires a second frame header right after the first).
// Returns -1 when none is found in the first `len` bytes.
inline long firstFrame(const uint8_t *b, size_t len, Frame &out) {
	size_t pos = nmid3::v2Size(b, len);
	for (size_t scanned = 0; pos + 4 <= len && scanned < 16384; pos++, scanned++) {
		Frame f = header(b + pos);
		if (!f.ok)
			continue;
		const size_t nxt = pos + (size_t)f.bytes;
		if (nxt + 4 <= len && !header(b + nxt).ok)
			continue;
		out = f;
		return (long)pos;
	}
	return -1;
}

// Duration from a Xing/Info frame count when present, else from the bitrate and the audio byte count (exact for CBR).
inline uint32_t durationMs(const uint8_t *b, size_t len, long frameOff, const Frame &f, uint32_t fileSize) {
	if (frameOff >= 0) {
		const size_t side = f.version == 1 ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
		const size_t x = (size_t)frameOff + 4 + side;
		if (x + 12 <= len && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4)) && (b[x + 7] & 1)) {
			const uint32_t frames = ((uint32_t)b[x + 8] << 24) | (b[x + 9] << 16) | (b[x + 10] << 8) | b[x + 11];
			return (uint32_t)((uint64_t)frames * f.samples * 1000 / f.rate);
		}
	}
	if (!f.bitrateKbps || frameOff < 0 || fileSize <= (uint32_t)frameOff)
		return 0;
	uint32_t audio = fileSize - (uint32_t)frameOff;
	// an ID3v1 tail (128 bytes) is below one frame of error and is ignored
	return (uint32_t)((uint64_t)audio * 8 / (uint32_t)f.bitrateKbps);
}

} // namespace nmmp3
