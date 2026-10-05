#include "musiclib.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "common/nmid3.h"
#include "common/nmmp3.h"
#include "common/nmwav.h"
#include "common/systemdetails.h"

namespace musiclib {

namespace {

constexpr int MAX_DEPTH = 4;
constexpr int MAX_DIRS = 150;
constexpr size_t HEAD = 16384;

std::string root() { return sys().isRunFromSD() ? "sd:" : "fat:"; }
std::string cacheDir() { return root() + "/_nds/nerdMod/cache"; }
std::string cacheFile() { return cacheDir() + "/music-index.txt"; }

void ensureDir(const std::string &d) { mkdir(d.c_str(), 0777); }

// Reads tags and duration of one file. Returns false when the file is not playable at all.
bool readMeta(nmmusic::Track &t) {
	FILE *f = fopen(t.path, "rb");
	if (!f)
		return false;
	uint8_t *buf = (u8 *)malloc(HEAD);
	if (!buf) {
		fclose(f);
		return false;
	}
	size_t n = fread(buf, 1, HEAD, f);
	nmid3::Tags tags;
	bool ok = false;
	if (t.kind == nmmusic::Kind::Wav) {
		const nmwav::Info w = nmwav::parse(buf, n, t.size);
		if (w.ok && w.rate >= 3000 && w.rate <= 48000) {
			t.durationMs = w.durationMs();
			ok = true;
		}
	} else if (t.kind == nmmusic::Kind::Mp3) {
		nmid3::parseV2(buf, n, tags);
		long fileOff = 0;
		nmmp3::Frame fr;
		long off = nmmp3::firstFrame(buf, n, fr);
		const uint32_t sz = nmid3::v2Size(buf, n);
		size_t len = n;
		if (off < 0 && sz >= n && sz < t.size) { // the tag (cover art) is bigger than the window
			fseek(f, (long)sz, SEEK_SET);
			len = fread(buf, 1, HEAD, f);
			fileOff = (long)sz;
			if (len >= 8) {
				off = nmmp3::firstFrame(buf, len, fr);
				// the buffer now starts after the tag: firstFrame found no ID3 there, off is relative to it
			}
		}
		if (off >= 0) {
			t.durationMs = nmmp3::durationMs(buf, len, off, fr, t.size - (uint32_t)fileOff);
			ok = true;
		}
		if (!tags.any() && t.size > 128) {
			uint8_t tail[128];
			if (fseek(f, (long)t.size - 128, SEEK_SET) == 0 && fread(tail, 1, 128, f) == 128)
				nmid3::parseV1(tail, tags);
		}
	}
	fclose(f);
	free(buf);
	if (!ok)
		return false;
	if (!tags.title[0])
		nmid3::fromFilename(t.path, tags);
	snprintf(t.title, sizeof(t.title), "%s", tags.title);
	snprintf(t.artist, sizeof(t.artist), "%s", tags.artist);
	snprintf(t.album, sizeof(t.album), "%s", tags.album);
	nmmusic::clean(t.title);
	nmmusic::clean(t.artist);
	nmmusic::clean(t.album);
	return true;
}

struct Ctx {
	nmmusic::Track *out;
	int n = 0;
	int dirs = 0;
	const nmmusic::Track *cache = nullptr;
	int cacheN = 0;
	ScanResult res;
	void (*progress)(int) = nullptr;
};

void walk(Ctx &c, const std::string &dir, int depth) {
	if (c.n >= nmmusic::MAX_TRACKS || c.dirs >= MAX_DIRS) {
		c.res.truncated = true;
		return;
	}
	c.dirs++;
	DIR *d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::string> subdirs;
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] == '.')
			continue;
		const std::string full = dir + "/" + e->d_name;
		if (e->d_type == DT_DIR) {
			if (depth < MAX_DEPTH)
				subdirs.push_back(full);
			continue;
		}
		if (nmmusic::kindOfPath(e->d_name) == nmmusic::Kind::Unknown)
			continue;
		if (c.n >= nmmusic::MAX_TRACKS) {
			c.res.truncated = true;
			break;
		}
		if (full.size() >= (size_t)nmmusic::PATH_MAX_LEN)
			continue;
		struct stat stt;
		if (stat(full.c_str(), &stt) != 0 || stt.st_size <= 0)
			continue;
		nmmusic::Track t;
		memset(&t, 0, sizeof(t));
		snprintf(t.path, sizeof(t.path), "%s", full.c_str());
		t.size = (uint32_t)stt.st_size;
		t.kind = nmmusic::kindOfPath(t.path);
		const nmmusic::Track *hit = nmmusic::findCached(c.cache, c.cacheN, t.path, t.size);
		if (hit) {
			t = *hit;
		} else {
			c.res.added++;
			if (!readMeta(t)) {
				c.res.unreadable++;
				c.res.added--;
				continue;
			}
		}
		c.out[c.n++] = t;
		if (c.progress && (c.n % 8) == 0)
			c.progress(c.n);
	}
	closedir(d);
	for (const std::string &s : subdirs)
		walk(c, s, depth + 1);
}

int loadCache(nmmusic::Track *dst, int cap) {
	FILE *f = fopen(cacheFile().c_str(), "rb");
	if (!f)
		return 0;
	int n = 0;
	char line[512];
	while (n < cap && fgets(line, sizeof(line), f)) {
		nmmusic::Track t;
		if (nmmusic::parseLine(line, t))
			dst[n++] = t;
	}
	fclose(f);
	return n;
}

bool saveCache(const nmmusic::Track *t, int n) {
	ensureDir(root() + "/_nds");
	ensureDir(root() + "/_nds/nerdMod");
	ensureDir(cacheDir());
	const std::string tmp = cacheFile() + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	char line[512];
	bool ok = true;
	for (int i = 0; i < n && ok; i++) {
		const int len = nmmusic::formatLine(t[i], line, sizeof(line));
		ok = len > 0 && len < (int)sizeof(line) && fwrite(line, 1, (size_t)len, f) == (size_t)len;
	}
	ok = (fclose(f) == 0) && ok;
	if (!ok)
		return false;
	remove(cacheFile().c_str());
	return rename(tmp.c_str(), cacheFile().c_str()) == 0;
}

} // namespace

ScanResult scan(nmmusic::Track *out, void (*progress)(int)) {
	Ctx c;
	c.out = out;
	c.progress = progress;
	nmmusic::Track *cache = (nmmusic::Track *)malloc(sizeof(nmmusic::Track) * nmmusic::MAX_TRACKS);
	if (cache)
		c.cacheN = loadCache(cache, nmmusic::MAX_TRACKS);
	c.cache = cache;
	const int cachedBefore = c.cacheN;

	walk(c, root() + "/Music", 0);
	walk(c, root() + "/_nds/nerdMod/music", 0);

	// removed = cached tracks whose path no longer shows up with the same size
	int kept = 0;
	for (int i = 0; i < cachedBefore; i++)
		for (int j = 0; j < c.n; j++)
			if (!strcmp(cache[i].path, out[j].path) && cache[i].size == out[j].size) {
				kept++;
				break;
			}
	c.res.removed = cachedBefore - kept;
	nmmusic::sortTracks(out, c.n);
	c.res.count = c.n;
	if (cache)
		free(cache);
	if (c.res.added || c.res.removed || cachedBefore != c.n)
		c.res.cacheWritten = saveCache(out, c.n);
	return c.res;
}

} // namespace musiclib
