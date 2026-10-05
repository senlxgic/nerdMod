// nerdMod Music: playback clock and ring arithmetic. Pure code, host-tested (camera/tools/hosttest/test_music.cpp).
#pragma once

#include <stdint.h>
#include <stdio.h>

namespace musicmath {

// The DS sound timer reloads every `period` ticks of 16.756991 MHz (libnds: 0x1000000 / rate). Using the REAL period (not the
// nominal rate) as the clock keeps the position exact: 0.2 % of rounding would otherwise drift a 0.37 s ring in a few minutes.
inline uint32_t periodFor(uint32_t rate) { return rate ? 0x1000000u / rate : 0; }

// msclock ticks (33513982 / 1024 Hz) -> frames the hardware has consumed: ticks * 1024 / 33513982 s * 16756991 / period.
inline uint64_t framesFromTicks(uint32_t ticks, uint32_t period) { return period ? (uint64_t)ticks * 512u / period : 0; }

// Frames that may be written now. wf = frames written, pf = frames played (both since the ring origin), margin = frames the
// hardware already latched that must not be touched. Never negative.
inline uint32_t ringFree(uint64_t wf, uint64_t pf, uint32_t ringFrames, uint32_t margin) {
	const uint64_t limit = pf + ringFrames - margin;
	return wf >= limit ? 0 : (uint32_t)(limit - wf);
}

// True when the hardware has run past everything that was written (the ring now repeats old audio).
inline bool underrun(uint64_t wf, uint64_t pf, uint32_t slack) { return pf > wf + slack; }

inline uint32_t mediaMs(uint64_t baseFrames, uint64_t playedFrames, uint32_t rate) {
	return rate ? (uint32_t)((baseFrames + playedFrames) * 1000u / rate) : 0;
}

// "=====>-----" of `width` characters for fraction permille 0..1000.
inline void bar(uint32_t permille, int width, char *out) {
	if (permille > 1000)
		permille = 1000;
	const int pos = (int)((uint64_t)permille * (uint32_t)(width - 1) / 1000);
	for (int i = 0; i < width; i++)
		out[i] = i < pos ? '=' : (i == pos ? '>' : '-');
	out[width] = 0;
}

// Touch x (pixels) over a bar that starts at pixel x0 and is w pixels wide -> permille.
inline uint32_t touchPermille(int x, int x0, int w) {
	if (w <= 0 || x <= x0)
		return 0;
	if (x >= x0 + w)
		return 1000;
	return (uint32_t)((x - x0) * 1000 / w);
}

inline int stepVolume(int v, int delta) {
	v += delta * 8;
	return v < 0 ? 0 : (v > 127 ? 127 : v);
}

} // namespace musicmath
