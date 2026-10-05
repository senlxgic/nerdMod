// nerdMod Music: track list model, metadata index cache and playback helpers. Pure code, host-tested.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmid3.h"

namespace nmmusic {

enum class Kind : uint8_t { Unknown, Mp3, Wav };

constexpr int MAX_TRACKS = 400;
constexpr int PATH_MAX_LEN = 112;

struct Track {
	char path[PATH_MAX_LEN];
	char title[nmid3::FIELD];
	char artist[nmid3::FIELD];
	char album[nmid3::FIELD];
	uint32_t size;
	uint32_t durationMs;
	Kind kind;
};

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

inline Kind kindOfPath(const char *path) {
	const char *dot = strrchr(path, '.');
	if (!dot)
		return Kind::Unknown;
	char e[5] = {0};
	for (int i = 0; i < 4 && dot[1 + i]; i++)
		e[i] = lower(dot[1 + i]);
	if (!strcmp(e, "mp3"))
		return Kind::Mp3;
	if (!strcmp(e, "wav"))
		return Kind::Wav;
	return Kind::Unknown;
}

// Case-insensitive ordering by artist, then title, then path (empty artists sort last).
inline int compare(const Track &a, const Track &b) {
	auto ci = [](const char *x, const char *y) {
		for (;; x++, y++) {
			const char cx = lower(*x), cy = lower(*y);
			if (cx != cy)
				return cx < cy ? -1 : 1;
			if (!cx)
				return 0;
		}
	};
	if ((a.artist[0] == 0) != (b.artist[0] == 0))
		return a.artist[0] ? -1 : 1;
	int r = ci(a.artist, b.artist);
	if (!r)
		r = ci(a.title, b.title);
	if (!r)
		r = ci(a.path, b.path);
	return r;
}

inline void sortTracks(Track *t, int n) {
	for (int i = 1; i < n; i++) { // insertion sort: n <= 400 and the list is mostly ordered after a cache hit
		Track x = t[i];
		int j = i - 1;
		while (j >= 0 && compare(t[j], x) > 0) {
			t[j + 1] = t[j];
			j--;
		}
		t[j + 1] = x;
	}
}

inline void clean(char *s) {
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

// Index cache, one track per line: path TAB size TAB durationMs TAB title TAB artist TAB album.
inline int formatLine(const Track &t, char *out, int cap) {
	return snprintf(out, (size_t)cap, "%s\t%lu\t%lu\t%s\t%s\t%s\n", t.path, (unsigned long)t.size, (unsigned long)t.durationMs, t.title, t.artist, t.album);
}

inline bool parseLine(char *line, Track &t) {
	char *f[6];
	int n = 0;
	char *p = line;
	f[n++] = p;
	while (*p && n < 6) {
		if (*p == '\t') {
			*p = 0;
			f[n++] = p + 1;
		}
		p++;
	}
	if (n != 6)
		return false;
	char *nl = strpbrk(f[5], "\r\n");
	if (nl)
		*nl = 0;
	if (!f[0][0] || strlen(f[0]) >= (size_t)PATH_MAX_LEN)
		return false;
	memset(&t, 0, sizeof(t));
	strcpy(t.path, f[0]);
	t.size = (uint32_t)strtoul(f[1], nullptr, 10);
	t.durationMs = (uint32_t)strtoul(f[2], nullptr, 10);
	snprintf(t.title, sizeof(t.title), "%s", f[3]);
	snprintf(t.artist, sizeof(t.artist), "%s", f[4]);
	snprintf(t.album, sizeof(t.album), "%s", f[5]);
	t.kind = kindOfPath(t.path);
	return t.kind != Kind::Unknown;
}

// A cached entry is reused only when the path and the size still match (a replaced file is re-read).
inline const Track *findCached(const Track *cache, int n, const char *path, uint32_t size) {
	for (int i = 0; i < n; i++)
		if (cache[i].size == size && !strcmp(cache[i].path, path))
			return &cache[i];
	return nullptr;
}

// "3:07" / "12:05"; "--:--" for unknown.
inline void formatTime(uint32_t ms, char *out, int cap) {
	if (ms == 0xFFFFFFFFu) {
		snprintf(out, (size_t)cap, "--:--");
		return;
	}
	const uint32_t s = ms / 1000;
	snprintf(out, (size_t)cap, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// Next track index: shuffle is a deterministic LCG-driven pick that never repeats the current track when n > 1.
inline int nextIndex(int cur, int n, bool shuffle, uint32_t &rng, int dir = 1) {
	if (n <= 0)
		return -1;
	if (!shuffle || n == 1)
		return (cur + dir + n) % n;
	rng = rng * 1664525u + 1013904223u;
	int k = (int)((rng >> 8) % (uint32_t)(n - 1));
	return (cur + 1 + k) % n;
}

} // namespace nmmusic
