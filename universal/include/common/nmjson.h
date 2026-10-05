/*
	nmjson: a small, bounded JSON reader for the nerdMod apps (weather answers). No allocation, no exceptions, no recursion
	beyond MAX_DEPTH, never reads outside the given text. It builds a node table (at most MAX_NODES nodes) and answers
	look-ups; anything malformed, too deep or too big makes parse() fail instead of guessing.

	Strings are returned as plain ASCII: escapes are decoded and non-ASCII letters are folded to the nearest ASCII letter
	(the 5x7 UI font has no accents), anything else becomes '?'.
*/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace nmjson {

enum Type : uint8_t { T_NULL = 0, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ };

struct Node {
	Type type = T_NULL;
	int16_t first = -1;	  // first child
	int16_t next = -1;	  // next sibling
	uint16_t count = 0;	  // children
	uint32_t keyOff = 0;  // key text (objects' children): offset/length into the source
	uint16_t keyLen = 0;
	uint32_t off = 0;	  // string text (raw, still escaped) offset/length into the source
	uint16_t len = 0;
	double num = 0;		  // number, or 0/1 for bool
};

// Folds a UTF-8 code point to ASCII ('u' for u-umlaut etc.); '?' when there is no sensible letter.
inline char foldCodePoint(uint32_t cp) {
	if (cp < 0x80)
		return (char)cp;
	static const char *latin1 = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"; // U+00C0..U+00FF
	if (cp >= 0xC0 && cp <= 0xFF)
		return latin1[cp - 0xC0] == '*' || latin1[cp - 0xC0] == '/' ? '?' : latin1[cp - 0xC0];
	return '?';
}

class Doc {
  public:
	static constexpr int MAX_NODES = 640;
	static constexpr int MAX_DEPTH = 8;

	// Parses `text` (not necessarily NUL terminated). The text must stay alive while the Doc is used.
	bool parse(const char *text, size_t length) {
		src = text;
		srcLen = length;
		used = 0;
		pos = 0;
		ok = true;
		if (!text || length == 0) {
			ok = false;
			return false;
		}
		skipWs();
		const int r = value(0, false);
		skipWs();
		if (r < 0 || pos != srcLen) {
			ok = false;
			used = 0;
			return false;
		}
		return true;
	}

	int root() const { return used > 0 ? 0 : -1; }
	Type type(int n) const { return n >= 0 && n < used ? nodes[n].type : T_NULL; }
	int size(int n) const { return n >= 0 && n < used ? nodes[n].count : 0; }

	// Value of key `key` in object `obj`, or -1.
	int get(int obj, const char *key) const {
		if (type(obj) != T_OBJ)
			return -1;
		const size_t kl = strlen(key);
		for (int c = nodes[obj].first; c >= 0; c = nodes[c].next)
			if (nodes[c].keyLen == kl && memcmp(src + nodes[c].keyOff, key, kl) == 0)
				return c;
		return -1;
	}
	// i-th element of array `arr`, or -1.
	int at(int arr, int i) const {
		if (type(arr) != T_ARR || i < 0)
			return -1;
		int c = nodes[arr].first;
		while (c >= 0 && i-- > 0)
			c = nodes[c].next;
		return c;
	}
	bool number(int n, double &out) const {
		if (type(n) != T_NUM)
			return false;
		out = nodes[n].num;
		return true;
	}
	bool boolean(int n, bool &out) const {
		if (type(n) != T_BOOL)
			return false;
		out = nodes[n].num != 0;
		return true;
	}
	// Decoded ASCII text, always NUL terminated, truncated to n-1 characters.
	bool string(int n, char *out, size_t cap) const {
		if (!out || cap == 0)
			return false;
		out[0] = 0;
		if (type(n) != T_STR)
			return false;
		size_t o = 0;
		const char *s = src + nodes[n].off;
		const size_t len = nodes[n].len;
		for (size_t i = 0; i < len && o + 1 < cap;) {
			unsigned char c = (unsigned char)s[i];
			if (c == '\\' && i + 1 < len) {
				const char e = s[i + 1];
				i += 2;
				switch (e) {
					case 'n': case 't': case 'r': out[o++] = ' '; break;
					case 'u': {
						uint32_t cp = 0;
						if (!hex4(s + i, len - i, cp))
							return false;
						i += 4;
						out[o++] = foldCodePoint(cp);
						break;
					}
					default: out[o++] = e; break; // \" \\ \/ \b \f
				}
			} else if (c < 0x80) {
				out[o++] = (char)(c < 0x20 ? ' ' : c);
				i++;
			} else {
				uint32_t cp = 0;
				int extra = 0;
				if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
				else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
				else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
				else { out[o++] = '?'; i++; continue; }
				i++;
				for (int k = 0; k < extra && i < len && ((unsigned char)s[i] & 0xC0) == 0x80; k++, i++)
					cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
				out[o++] = foldCodePoint(cp);
			}
		}
		out[o] = 0;
		return true;
	}

  private:
	static bool hex4(const char *s, size_t avail, uint32_t &cp) {
		if (avail < 4)
			return false;
		cp = 0;
		for (int i = 0; i < 4; i++) {
			const char c = s[i];
			cp <<= 4;
			if (c >= '0' && c <= '9') cp |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f') cp |= (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') cp |= (uint32_t)(c - 'A' + 10);
			else return false;
		}
		return true;
	}
	void skipWs() {
		while (pos < srcLen && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n'))
			pos++;
	}
	int alloc() {
		if (used >= MAX_NODES)
			return -1;
		nodes[used] = Node();
		return used++;
	}
	bool scanString(uint32_t &off, uint16_t &len) {
		if (pos >= srcLen || src[pos] != '"')
			return false;
		pos++;
		const size_t start = pos;
		while (pos < srcLen) {
			const unsigned char c = (unsigned char)src[pos];
			if (c == '"') {
				if (pos - start > 0xFFFF)
					return false;
				off = (uint32_t)start;
				len = (uint16_t)(pos - start);
				pos++;
				return true;
			}
			if (c < 0x20)
				return false;
			if (c == '\\') {
				pos++;
				if (pos >= srcLen)
					return false;
				const char e = src[pos];
				if (e == 'u') {
					uint32_t cp;
					if (!hex4(src + pos + 1, srcLen - pos - 1, cp))
						return false;
					pos += 4;
				} else if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't')) {
					return false;
				}
			}
			pos++;
		}
		return false;
	}
	bool literal(const char *w) {
		const size_t n = strlen(w);
		if (pos + n > srcLen || memcmp(src + pos, w, n) != 0)
			return false;
		pos += n;
		return true;
	}
	// Parses one value; returns its node index or -1.
	int value(int depth, bool) {
		if (depth > MAX_DEPTH || pos >= srcLen)
			return -1;
		const int n = alloc();
		if (n < 0)
			return -1;
		const char c = src[pos];
		if (c == '{' || c == '[') {
			const bool obj = c == '{';
			nodes[n].type = obj ? T_OBJ : T_ARR;
			pos++;
			skipWs();
			int last = -1;
			if (pos < srcLen && src[pos] == (obj ? '}' : ']')) {
				pos++;
				return n;
			}
			while (true) {
				skipWs();
				uint32_t kOff = 0;
				uint16_t kLen = 0;
				if (obj) {
					if (!scanString(kOff, kLen))
						return -1;
					skipWs();
					if (pos >= srcLen || src[pos] != ':')
						return -1;
					pos++;
					skipWs();
				}
				const int child = value(depth + 1, false);
				if (child < 0)
					return -1;
				nodes[child].keyOff = kOff;
				nodes[child].keyLen = kLen;
				if (last < 0)
					nodes[n].first = (int16_t)child;
				else
					nodes[last].next = (int16_t)child;
				last = child;
				nodes[n].count++;
				skipWs();
				if (pos >= srcLen)
					return -1;
				if (src[pos] == ',') {
					pos++;
					continue;
				}
				if (src[pos] == (obj ? '}' : ']')) {
					pos++;
					return n;
				}
				return -1;
			}
		}
		if (c == '"') {
			nodes[n].type = T_STR;
			return scanString(nodes[n].off, nodes[n].len) ? n : -1;
		}
		if (c == 't' && literal("true")) { nodes[n].type = T_BOOL; nodes[n].num = 1; return n; }
		if (c == 'f' && literal("false")) { nodes[n].type = T_BOOL; nodes[n].num = 0; return n; }
		if (c == 'n' && literal("null")) { nodes[n].type = T_NULL; return n; }
		if (c == '-' || (c >= '0' && c <= '9')) {
			// strict JSON number grammar, bounded length
			size_t p = pos;
			char buf[40];
			size_t bl = 0;
			if (src[p] == '-') buf[bl++] = src[p++];
			if (p >= srcLen || !(src[p] >= '0' && src[p] <= '9'))
				return -1;
			while (p < srcLen && bl < sizeof(buf) - 1 && ((src[p] >= '0' && src[p] <= '9') || src[p] == '.' || src[p] == 'e' || src[p] == 'E' || src[p] == '+' || src[p] == '-'))
				buf[bl++] = src[p++];
			if (p < srcLen && ((src[p] >= '0' && src[p] <= '9') || src[p] == '.'))
				return -1; // too long
			buf[bl] = 0;
			char *end = nullptr;
			const double v = strtod(buf, &end);
			if (end == buf || *end != 0 || !(v > -1e300 && v < 1e300)) // also rejects inf/nan
				return -1;
			nodes[n].type = T_NUM;
			nodes[n].num = v;
			pos = p;
			return n;
		}
		return -1;
	}

	const char *src = nullptr;
	size_t srcLen = 0, pos = 0;
	int used = 0;
	bool ok = false;
	Node nodes[MAX_NODES];
};

} // namespace nmjson
