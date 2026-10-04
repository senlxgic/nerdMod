// Host tests of the playback ring placement (audioring.h).
#include <stdio.h>
#include "../../arm9/source/audioring.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace audioring;

int main() {
	const uint32_t R = 131072, M = 1024;
	const uint32_t s0 = 5000 * 32; // started at 5 s
	// first chunk exactly at the start
	Placement p = place(s0, s0, s0, 8192, R, M);
	CHECK(!p.drop);
	CHECK(p.skip == M && p.count == 8192 - M && p.ringIndex == M);
	// a chunk ahead of the play position goes in untouched
	p = place(s0, s0 + 3200, s0 + 16000, 8192, R, M);
	CHECK(!p.drop && p.skip == 0 && p.count == 8192 && p.ringIndex == 16000);
	// wrap: ring index wraps modulo ring size
	p = place(s0, s0 + 100000, s0 + 130000, 8192, R, M);
	CHECK(!p.drop && p.ringIndex == 130000 % R);
	// late chunk (entirely played) is dropped, not played over later audio
	p = place(s0, s0 + 50000, s0 + 10000, 8192, R, M);
	CHECK(p.drop);
	// partly late: front is cut, stays even
	p = place(s0, s0 + 10000, s0 + 8000, 8192, R, M);
	CHECK(!p.drop && (p.skip % 2) == 0 && p.skip == 10000 + M - 8000 && p.count == 8192 - p.skip);
	// too far ahead (would overwrite unplayed data) is dropped
	p = place(s0, s0, s0 + R, 4096, R, M);
	CHECK(p.drop);
	// near the far limit: truncated, never beyond limit
	p = place(s0, s0, s0 + R - 4096, 8192, R, M);
	CHECK(p.count > 0 && p.count < 8192);
	// before the start of the stream
	p = place(s0, s0, s0 - 4000, 8192, R, M);
	CHECK(!p.drop && p.skip == 4000 + M);
	// odd sizes are rounded down to whole samples
	p = place(s0, s0, s0 + 8000, 1001, R, M);
	CHECK(!p.drop && p.count == 1000);
	// zero
	CHECK(place(s0, s0, s0, 0, R, M).drop);
	// continuous stream written chunk by chunk maps to contiguous ring bytes
	uint32_t pos = s0 + 2000, consumed = s0;
	uint32_t prevEnd = 0;
	for (int i = 0; i < 20; i++) {
		p = place(s0, consumed, pos, 8192, R, M);
		CHECK(!p.drop);
		if (i) CHECK(p.ringIndex == prevEnd % R);
		prevEnd = p.ringIndex + p.count;
		pos += 8192;
		consumed += 8192;
	}
	printf(failures ? "%d FAILURES\n" : "audio ring tests OK\n", failures);
	return failures ? 1 : 0;
}
