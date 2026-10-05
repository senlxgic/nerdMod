/*
	SD write benchmark (Phase 2D). Runs from Camera > Settings > SD Speed Test and never while recording.

	It writes the same shapes the video recorder uses, and a few alternatives, to a temporary file under
	<sd>:/_nds/nerdMod/cache/, times every write() with the free-running microsecond clock and writes a report to
	<sd>:/_nds/nerdMod/sd-benchmark.txt. The report file is ALWAYS attempted (a "started" stub is written first), and
	if it cannot be written the failure is kept in Report::writeError so the screen can show it.

	Tests: growth writes of 16/32/64/96/98(recorder slot)/128/256/512 KiB, an unaligned 98,336-byte run (the Phase 2B/2C
	recorder), in-place rewrites of the file (no cluster allocation), and growth writes with the microphone capture
	running, which separates "the card is slow" from "the ARM7 is busy with the microphone". No per-write fsync: one
	fsync() at the end of each test is timed separately (flush_ms).
*/
#pragma once

#include <nds.h>

#include "sdbenchcalc.h"

namespace sdbench {

constexpr int MAX_TESTS = 14;

struct Report {
	sdbenchcalc::Result tests[MAX_TESTS];
	int count = 0;
	char device[8] = "";
	char filesystem[16] = "";
	u32 clusterBytes = 0;
	char path[64] = "";		 // where the report was supposed to go
	bool wroteReport = false;
	char writeError[48] = ""; // why the report could not be written (empty when it was)
	bool cancelled = false;
};

// progress(name, index, total) runs before each test (may be null); cancel() is polled between writes (may be null).
void run(Report &out, void (*progress)(const char *name, int index, int total), bool (*cancel)());

// MB/s x100 of a test.
u32 mbPerSecX100(const sdbenchcalc::Result &r);

} // namespace sdbench
