/*
	A free-running millisecond clock for recording and playback: ARM9 timers 2 and 3 in cascade (32.7 kHz, 32 bit,
	wraps after 36 hours). Independent of vblank, so it keeps counting while the main loop is blocked in an SD write.
*/
#pragma once

#include <nds.h>

namespace msclock {

inline void start() {
	TIMER_CR(2) = 0;
	TIMER_CR(3) = 0;
	TIMER_DATA(2) = 0;
	TIMER_DATA(3) = 0;
	TIMER_CR(3) = TIMER_ENABLE | TIMER_CASCADE;
	TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_1024;
}

// Starts the clock only if it is not already running (does not disturb a running recording).
inline void ensureStarted() {
	if (!(TIMER_CR(3) & TIMER_ENABLE) || !(TIMER_CR(2) & TIMER_ENABLE))
		start();
}

inline void stop() {
	TIMER_CR(2) = 0;
	TIMER_CR(3) = 0;
}

inline u32 ticks() {
	u16 hi, lo, hi2;
	do {
		hi = TIMER_DATA(3);
		lo = TIMER_DATA(2);
		hi2 = TIMER_DATA(3); // if the low half rolled over between the reads, try again
	} while (hi != hi2);
	return ((u32)hi << 16) | lo;
}

// 33513982 Hz / 1024 = 32728.5 ticks per second
inline u32 toMs(u32 tickDelta) { return (u32)(((u64)tickDelta * 1024000ull) / 33513982ull); }

// microseconds (resolution about 30 us)
inline u32 toUs(u32 tickDelta) { return (u32)(((u64)tickDelta * 1024000000ull) / 33513982ull); }

} // namespace msclock
