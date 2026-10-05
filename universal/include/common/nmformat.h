// nerdMod: small pure helpers shared by the menu widgets (no libnds, host-tested in tools/hosttest).
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace nmformat {

// A single session longer than this is not believed (power-off, crash, forgotten console): it earns no play time.
constexpr uint32_t MAX_SESSION_SECONDS = 12u * 3600u;
// Clocks before 2020-01-01 mean the RTC was never set.
constexpr int64_t MIN_VALID_UNIX = 1577836800;

// Seconds to credit for a session that started at `start` and was noticed to be over at `now` (0 = not credited).
inline uint32_t sessionSeconds(int64_t start, int64_t now) {
	if (start < MIN_VALID_UNIX || now < MIN_VALID_UNIX || now <= start)
		return 0; // unset or moved-back clock
	const int64_t d = now - start;
	return d > (int64_t)MAX_SESSION_SECONDS ? 0 : (uint32_t)d;
}

// "18h 42m", "42m", "<1m"; hours are not wrapped into days so the figure stays honest.
inline void duration(char *out, size_t n, uint32_t seconds) {
	const uint32_t m = seconds / 60;
	if (m == 0)
		snprintf(out, n, "<1m");
	else if (m < 60)
		snprintf(out, n, "%um", (unsigned)m);
	else
		snprintf(out, n, "%uh %um", (unsigned)(m / 60), (unsigned)(m % 60));
}

// "just now", "5m ago", "2h ago", "3d ago"; a timestamp in the future or unset gives "".
inline void ago(char *out, size_t n, int64_t then, int64_t now) {
	out[0] = 0;
	if (then < MIN_VALID_UNIX || now < then)
		return;
	const int64_t d = now - then;
	if (d < 90)
		snprintf(out, n, "just now");
	else if (d < 3600)
		snprintf(out, n, "%dm ago", (int)(d / 60));
	else if (d < 48 * 3600)
		snprintf(out, n, "%dh ago", (int)(d / 3600));
	else
		snprintf(out, n, "%dd ago", (int)(d / 86400));
}

// "seconds,lastPlayed" <-> numbers (a damaged value reads as 0,0).
inline void parsePair(const char *s, uint32_t &seconds, int64_t &last) {
	seconds = 0;
	last = 0;
	if (!s)
		return;
	unsigned long long a = 0, b = 0;
	if (sscanf(s, "%llu,%llu", &a, &b) >= 1) {
		seconds = a > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (uint32_t)a;
		last = (int64_t)b;
	}
}

// Phase 2D Play Stats wording. Play time: under a minute "<1 min", minutes "12 min", hours "1h 32m" (never a bare "1M").
inline void playTime(char *out, size_t n, uint32_t seconds) {
	const uint32_t m = seconds / 60;
	if (m == 0)
		snprintf(out, n, "<1 min");
	else if (m < 60)
		snprintf(out, n, "%u min", (unsigned)m);
	else
		snprintf(out, n, "%uh %um", (unsigned)(m / 60), (unsigned)(m % 60));
}

// "Played 1 time" / "Played 12 times" / "Never played"
inline void playedTimes(char *out, size_t n, uint32_t launches) {
	if (launches == 0)
		snprintf(out, n, "Never played");
	else
		snprintf(out, n, "Played %u %s", (unsigned)launches, launches == 1 ? "time" : "times");
}

// "Today", "Yesterday", "3 days ago", "2 months ago"; "" when the timestamp is unset or in the future. Calendar days of the RTC.
inline void lastPlayedDay(char *out, size_t n, int64_t last, int64_t now) {
	out[0] = 0;
	if (last < MIN_VALID_UNIX || now < MIN_VALID_UNIX || now < last)
		return;
	const int64_t days = now / 86400 - last / 86400;
	if (days == 0)
		snprintf(out, n, "Today");
	else if (days == 1)
		snprintf(out, n, "Yesterday");
	else if (days < 45)
		snprintf(out, n, "%d days ago", (int)days);
	else
		snprintf(out, n, "%d months ago", (int)(days / 30));
}

// Title -> lines of at most `width` characters (word wrap, long words broken, extension and trailing junk dropped),
// at most `maxLines`; the last line ends in ".." when text was cut. Returns the number of lines.
inline int wrapTitle(const char *title, int width, int maxLines, char lines[][24]) {
	char buf[96];
	snprintf(buf, sizeof(buf), "%s", title ? title : "");
	// drop a known file extension
	const char *exts[] = {".nds", ".dsi", ".srl", ".gba", ".gbc", ".gb", ".nes", ".sms", ".gg", ".md", ".smd", ".gen", ".sfc", ".smc", ".snes", ".app", ".srldr", ".ids", ".xex"};
	const size_t bl = strlen(buf);
	for (const char *e : exts) {
		const size_t el = strlen(e);
		if (bl > el) {
			bool match = true;
			for (size_t i = 0; i < el; i++) {
				char c = buf[bl - el + i];
				if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
				if (c != e[i]) { match = false; break; }
			}
			if (match) { buf[bl - el] = 0; break; }
		}
	}
	if (width > 22) width = 22;
	int count = 0;
	size_t i = 0;
	const size_t len = strlen(buf);
	while (i < len && count < maxLines) {
		while (i < len && buf[i] == ' ') i++;
		if (i >= len) break;
		size_t take = len - i < (size_t)width ? len - i : (size_t)width;
		if (take == (size_t)width && i + take < len) {
			// prefer breaking at a space inside the line
			size_t brk = take;
			while (brk > 0 && buf[i + brk] != ' ') brk--;
			if (brk > 0) take = brk;
		}
		size_t j = take;
		while (j > 0 && buf[i + j - 1] == ' ') j--;
		memcpy(lines[count], buf + i, j);
		lines[count][j] = 0;
		count++;
		i += take;
	}
	while (i < len && buf[i] == ' ') i++;
	if (i < len && count > 0) {
		// text left over: mark the cut
		char *l = lines[count - 1];
		size_t ll = strlen(l);
		if (ll + 2 > (size_t)width) ll = (size_t)width - 2;
		while (ll > 0 && l[ll - 1] == ' ') ll--;
		l[ll] = '.'; l[ll + 1] = '.'; l[ll + 2] = 0;
	}
	return count;
}

} // namespace nmformat
