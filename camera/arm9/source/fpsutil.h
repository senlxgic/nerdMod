/*
	Frame-rate arithmetic of the nerdMod video recorder. No dependencies, so the host tests can include it.

	Frame k of a recording at F fps is due at k * 1000 / F milliseconds. Everything is integer maths on the millisecond
	clock (msclock), so the grid cannot drift: the error never accumulates, it is always < 1 ms.
*/
#pragma once

#include <stdint.h>

namespace fpsutil {

constexpr int RATES[4] = {10, 15, 20, 30};
constexpr int RATE_COUNT = 4;
constexpr int DEFAULT_FPS = 10;

inline bool valid(int fps) {
	for (int r : RATES)
		if (r == fps)
			return true;
	return false;
}

inline int sanitize(int fps) { return valid(fps) ? fps : DEFAULT_FPS; }

inline int next(int fps) {
	for (int i = 0; i < RATE_COUNT; i++)
		if (RATES[i] == fps)
			return RATES[(i + 1) % RATE_COUNT];
	return DEFAULT_FPS;
}

inline const char *label(int fps) {
	switch (fps) {
		case 15: return "15 FPS - Smooth";
		case 20: return "20 FPS - High";
		case 30: return "30 FPS - Max";
		default: return "10 FPS - Safe";
	}
}

// Nominal storage in decimal MB per minute: 98304 bytes per RGB555 frame plus 32000 bytes/s of 16 kHz PCM16 audio.
inline int mbPerMinute(int fps) { return (int)((((uint64_t)98304 * (uint64_t)fps + 32000u) * 60u + 500000u) / 1000000u); }

// Exact due time (ms) of frame k.
inline uint32_t dueMs(uint32_t k, int fps) { return (uint32_t)(((uint64_t)k * 1000u) / (uint32_t)fps); }

// Slot of the grid nearest to time t (ms).
inline uint32_t nearestIndex(uint32_t tMs, int fps) { return (uint32_t)(((uint64_t)tMs * (uint32_t)fps + 500u) / 1000u); }

// How early a camera frame may arrive and still be taken for slot k: half an interval, at most 20 ms.
inline uint32_t slackMs(int fps) {
	const uint32_t half = (1000u / (uint32_t)fps) / 2u;
	return half < 20u ? half : 20u;
}

inline bool isDue(uint32_t nowMs, uint32_t k, int fps) { return nowMs + slackMs(fps) >= dueMs(k, fps); }

// Frames the grid holds for a recording of the given length.
inline uint32_t expectedFrames(uint32_t durationMs, int fps) { return nearestIndex(durationMs, fps); }

// Actual average rate, in hundredths of a frame per second.
inline uint32_t averageFpsX100(uint32_t frames, uint32_t durationMs) {
	if (!durationMs)
		return 0;
	return (uint32_t)(((uint64_t)frames * 100000u + durationMs / 2u) / durationMs);
}

// Minutes of recording that fit in `bytes` at this rate (limited by the 1500 MiB cap and the 30 minute cap).
inline uint32_t secondsThatFit(uint64_t bytes, int fps) {
	const uint64_t perSec = (uint64_t)98304 * (uint64_t)fps + 32000u; // + 16 kHz PCM16 audio
	return (uint32_t)(bytes / perSec);
}

// Parses "VIDEO_FPS=NN" out of a small INI text. Returns the default when missing or invalid.
inline int parseFps(const char *text) {
	if (!text)
		return DEFAULT_FPS;
	const char *key = "VIDEO_FPS=";
	for (const char *p = text; *p; p++) {
		bool atLineStart = (p == text) || p[-1] == '\n';
		if (!atLineStart)
			continue;
		int i = 0;
		while (key[i] && p[i] == key[i])
			i++;
		if (key[i])
			continue;
		int v = 0, digits = 0;
		const char *d = p + i;
		while (*d >= '0' && *d <= '9' && digits < 4) {
			v = v * 10 + (*d - '0');
			d++;
			digits++;
		}
		return digits ? sanitize(v) : DEFAULT_FPS;
	}
	return DEFAULT_FPS;
}

} // namespace fpsutil
