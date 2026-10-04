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

} // namespace nmformat
