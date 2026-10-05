// nerdMod Music: ID3v2 / ID3v1 tag reading with a filename fallback. Output is plain ASCII (the home font is ASCII only).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace nmid3 {

constexpr int FIELD = 40;

struct Tags {
	char title[FIELD] = "";
	char artist[FIELD] = "";
	char album[FIELD] = "";
	bool any() const { return title[0] || artist[0] || album[0]; }
};

// Total size of an ID3v2 tag at the start of buf (header + body), or 0 when there is none.
inline uint32_t v2Size(const uint8_t *b, size_t len) {
	if (len < 10 || memcmp(b, "ID3", 3) != 0 || b[3] == 0xFF || b[3] < 2)
		return 0;
	for (int i = 6; i < 10; i++)
		if (b[i] & 0x80)
			return 0;
	const uint32_t body = ((uint32_t)b[6] << 21) | ((uint32_t)b[7] << 14) | ((uint32_t)b[8] << 7) | b[9];
	return 10 + body + ((b[5] & 0x10) ? 10 : 0);
}

inline void putText(char *dst, const uint8_t *s, size_t n, int enc) {
	size_t o = 0;
	auto push = [&](unsigned c) {
		if (o + 1 >= (size_t)FIELD)
			return;
		if (c == 0)
			return;
		dst[o++] = (c >= 32 && c < 127) ? (char)c : '?';
	};
	if (enc == 1 || enc == 2) { // UTF-16 (with BOM / big endian)
		bool be = enc == 2;
		size_t i = 0;
		if (enc == 1 && n >= 2) {
			if (s[0] == 0xFE && s[1] == 0xFF) {
				be = true;
				i = 2;
			} else if (s[0] == 0xFF && s[1] == 0xFE) {
				i = 2;
			}
		}
		for (; i + 1 < n; i += 2) {
			const unsigned lo = be ? s[i + 1] : s[i], hi = be ? s[i] : s[i + 1];
			if (lo == 0 && hi == 0)
				break;
			push(hi ? '?' : lo);
		}
	} else if (enc == 3) { // UTF-8: ASCII is kept, a multi-byte sequence becomes one '?'
		for (size_t i = 0; i < n && s[i]; i++) {
			if (s[i] < 0x80)
				push(s[i]);
			else if (s[i] >= 0xC0)
				push('?');
		}
	} else { // ISO-8859-1
		for (size_t i = 0; i < n && s[i]; i++)
			push(s[i]);
	}
	dst[o] = 0;
	while (o && dst[o - 1] == ' ')
		dst[--o] = 0;
}

// Reads TIT2 / TPE1 / TALB (v2.3 / v2.4) or TT2 / TP1 / TAL (v2.2) from the tag in buf (len bytes available).
inline bool parseV2(const uint8_t *b, size_t len, Tags &t) {
	const uint32_t total = v2Size(b, len);
	if (!total)
		return false;
	const int ver = b[3];
	size_t end = total < len ? total : len;
	size_t pos = 10;
	if (b[5] & 0x40) { // extended header
		if (pos + 4 > end)
			return false;
		const uint32_t es = ver == 4 ? (((uint32_t)b[10] << 21) | ((uint32_t)b[11] << 14) | ((uint32_t)b[12] << 7) | b[13]) : (((uint32_t)b[10] << 24) | (b[11] << 16) | (b[12] << 8) | b[13]) + 4;
		pos += es;
	}
	const size_t hdr = ver == 2 ? 6 : 10;
	bool found = false;
	while (pos + hdr <= end) {
		const uint8_t *f = b + pos;
		if (f[0] == 0)
			break; // padding
		uint32_t sz;
		if (ver == 2)
			sz = ((uint32_t)f[3] << 16) | (f[4] << 8) | f[5];
		else if (ver == 4)
			sz = ((uint32_t)f[4] << 21) | ((uint32_t)f[5] << 14) | ((uint32_t)f[6] << 7) | f[7];
		else
			sz = ((uint32_t)f[4] << 24) | (f[5] << 16) | (f[6] << 8) | f[7];
		if (sz == 0 || pos + hdr + sz > end)
			break;
		char *dst = nullptr;
		if (ver == 2) {
			if (!memcmp(f, "TT2", 3)) dst = t.title;
			else if (!memcmp(f, "TP1", 3)) dst = t.artist;
			else if (!memcmp(f, "TAL", 3)) dst = t.album;
		} else {
			if (!memcmp(f, "TIT2", 4)) dst = t.title;
			else if (!memcmp(f, "TPE1", 4)) dst = t.artist;
			else if (!memcmp(f, "TALB", 4)) dst = t.album;
		}
		if (dst) {
			const uint8_t *body = f + hdr;
			putText(dst, body + 1, sz - 1, body[0]);
			found = true;
		}
		pos += hdr + sz;
	}
	return found;
}

// `b` points at the last 128 bytes of the file.
inline bool parseV1(const uint8_t *b, Tags &t) {
	if (memcmp(b, "TAG", 3) != 0)
		return false;
	putText(t.title, b + 3, 30, 0);
	putText(t.artist, b + 33, 30, 0);
	putText(t.album, b + 63, 30, 0);
	return t.any();
}

// "/Music/Some Artist - A Song.mp3" -> title "A Song", artist "Some Artist". Without " - " the whole name is the title.
inline void fromFilename(const char *path, Tags &t) {
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char name[FIELD * 3];
	size_t n = strlen(base);
	const char *dot = strrchr(base, '.');
	if (dot && dot != base)
		n = (size_t)(dot - base);
	if (n >= sizeof(name))
		n = sizeof(name) - 1;
	memcpy(name, base, n);
	name[n] = 0;
	for (size_t i = 0; i < n; i++)
		if (name[i] == '_')
			name[i] = ' ';
	const char *sep = strstr(name, " - ");
	if (sep && sep != name && sep[3]) {
		const size_t an = (size_t)(sep - name);
		putText(t.artist, (const uint8_t *)name, an, 0);
		putText(t.title, (const uint8_t *)sep + 3, strlen(sep + 3), 0);
	} else {
		putText(t.title, (const uint8_t *)name, n, 0);
	}
}

} // namespace nmid3
