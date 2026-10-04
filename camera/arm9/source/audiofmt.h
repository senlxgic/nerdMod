// Sample-format helpers for the microphone path (pure, host-tested).
#pragma once

#include <stdint.h>

namespace audiofmt {

// libnds delivers 12-bit microphone samples shifted up to 16 bit. Depending on the path (NTR SPI or TWL codec) they
// may be centred on 0 (signed) or on 0x8000 (offset binary). Returns true when the block is offset binary, i.e. its
// signed mean is far from zero. Silence (all zero) is signed by definition.
inline bool looksOffsetBinary(const int16_t *s, uint32_t n) {
	if (!n)
		return false;
	int64_t sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum += s[i];
	const int64_t mean = sum / (int64_t)n;
	return mean > 20000 || mean < -20000;
}

inline void flipToSigned(int16_t *s, uint32_t n) {
	uint16_t *u = (uint16_t *)s;
	for (uint32_t i = 0; i < n; i++)
		u[i] ^= 0x8000u;
}

// Peak magnitude of a block (0..32768).
inline uint32_t peak(const int16_t *s, uint32_t n) {
	uint32_t p = 0;
	for (uint32_t i = 0; i < n; i++) {
		const int32_t v = s[i];
		const uint32_t a = (uint32_t)(v < 0 ? -v : v);
		if (a > p)
			p = a;
	}
	return p;
}

} // namespace audiofmt
