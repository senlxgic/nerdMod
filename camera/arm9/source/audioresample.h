/*
	Linear-interpolating sample-rate converter for the microphone path (host-tested, no dependencies).

	The DSi microphone controller samples at the codec rate divided by two: 16364 Hz (codec 32.7 kHz) or 23802 Hz (codec
	47.6 kHz). The NERDVID audio track is 16000 Hz, so the recorder converts while it copies. Position is tracked in
	Q32 fixed point, so no long-term drift: output k always sits at k * (inRate / outRate) input samples.
*/
#pragma once

#include <stdint.h>

namespace audioresample {

struct Linear {
	uint64_t step = 1ull << 32; // Q32: input samples consumed per output sample
	uint64_t pos = 0;			// Q32 position of the next output sample relative to `prev`
	int32_t prev = 0;
	bool havePrev = false;

	void init(uint32_t inRate, uint32_t outRate) {
		step = (((uint64_t)inRate << 32) + outRate / 2) / outRate;
		pos = 0;
		prev = 0;
		havePrev = false;
	}

	// Pushes one input sample, calls emit(int16_t) for every output sample that falls before it.
	template <class F> void push(int16_t in, F emit) {
		if (!havePrev) {
			prev = in;
			havePrev = true;
			emit(in); // the first output sample is the first input sample itself
			pos = step;
			return;
		}
		// outputs located between prev (index i-1) and in (index i): positions pos in [0, 1)
		while (pos <= (1ull << 32)) {
			const int64_t frac = (int64_t)(pos >> 16); // Q16 weight of the new sample (<= 65536)
			const int64_t v = (int64_t)prev * (65536 - frac) + (int64_t)in * frac;
			emit((int16_t)(v >> 16));
			pos += step;
		}
		pos -= (1ull << 32);
		prev = in;
	}
};

} // namespace audioresample
