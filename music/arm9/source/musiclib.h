// nerdMod Music: bounded library scan with a metadata cache (sd:/_nds/nerdMod/cache/music-index.txt).
#pragma once

#include <string>

#include "common/nmmusic.h"

namespace musiclib {

struct ScanResult {
	int count = 0;
	int added = 0;
	int removed = 0;
	int unreadable = 0;
	bool truncated = false; // more than MAX_TRACKS / visit limits hit
	bool cacheWritten = false;
};

// Scans sd:/Music, sd:/_nds/nerdMod/music (depth <= 4, <= 150 folders, <= nmmusic::MAX_TRACKS tracks). Reuses cached tags for
// files whose path and size did not change. `progress` is called every few files (may be null).
ScanResult scan(nmmusic::Track *out, void (*progress)(int found));

} // namespace musiclib
