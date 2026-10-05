/*
	SD write benchmark for the Camera diagnostics (Phase 2C.1). Writes a few megabytes in the same shapes the video recorder
	uses (and a few alternatives) to a temporary file in the video folder, times every write() with the millisecond clock and
	writes <videos>/sd-benchmark.txt. The temporary file is removed afterwards. Never runs while recording.

	Patterns: "unal98K" = 64-byte header then 98,336-byte writes (the Phase 2B / 2C recorder: every write starts mid-sector),
	"al98K" = 512-byte header then 98,816-byte writes (Phase 2C.1), "al32K" and "al256K" = the same at 32 KiB / 256 KiB,
	"rewrite" = the al98K file overwritten in place (no cluster allocation), which separates file growth from raw card speed.
*/
#pragma once

#include <nds.h>

namespace sdbench {

constexpr int PATTERNS = 5;

struct Row {
	char name[12] = "";
	u32 writes = 0;
	u32 bytes = 0;
	u32 totalMs = 0;
	u32 maxMs = 0;
	u32 closeMs = 0;
	bool ok = false;
};

struct Report {
	Row rows[PATTERNS];
	int count = 0;
	bool wroteFile = false;
};

// progress(name) is called before each pattern starts (may be null).
void run(Report &out, void (*progress)(const char *name));

// MB/s x100 of a row (bytes / time inside write()).
u32 mbPerSecX100(const Row &r);

} // namespace sdbench
