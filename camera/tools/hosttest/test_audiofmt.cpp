#include <stdio.h>
#include <vector>
#include <math.h>
#include "../../arm9/source/audiofmt.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int main() {
	std::vector<int16_t> sig(512), off(512), zero(512, 0);
	for (int i = 0; i < 512; i++) {
		sig[i] = (int16_t)(8000 * sin(i * 0.2));
		off[i] = (int16_t)((uint16_t)(sig[i]) ^ 0x8000u);
	}
	CHECK(!audiofmt::looksOffsetBinary(sig.data(), 512));
	CHECK(audiofmt::looksOffsetBinary(off.data(), 512));
	CHECK(!audiofmt::looksOffsetBinary(zero.data(), 512));
	CHECK(!audiofmt::looksOffsetBinary(zero.data(), 0));
	audiofmt::flipToSigned(off.data(), 512);
	CHECK(off == sig);
	CHECK(audiofmt::peak(sig.data(), 512) > 7900 && audiofmt::peak(sig.data(), 512) <= 8000);
	int16_t m = -32768;
	CHECK(audiofmt::peak(&m, 1) == 32768);
	printf(failures ? "%d FAILURES\n" : "audio format tests OK\n", failures);
	return failures;
}
