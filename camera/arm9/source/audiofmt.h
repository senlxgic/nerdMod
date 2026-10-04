// Sample-format helpers for the microphone path (pure, host-tested).
#pragma once

#include <stdint.h>

namespace audiofmt {

// libnds delivers 12-bit microphone samples shifted up to 16 bit. Depending on the path (NTR SPI or TWL codec) they
// may be centred on 0 (signed) or on 0x8000 (offset binary). Read as signed, an offset-binary block has a large
// mean magnitude (every sample sits near +-32768) while a real signal sits near 0. Silence (all zero) is signed.
inline bool looksOffsetBinary(const int16_t *s, uint32_t n) {
	if (!n)
		return false;
	uint64_t sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum += (uint64_t)(s[i] < 0 ? -(int32_t)s[i] : (int32_t)s[i]);
	return sum / n > 20000u;
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
