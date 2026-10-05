#include <stdio.h>
#include "../../../universal/include/common/nmformat.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace nmformat;
int main() {
	const int64_t T = 1700000000;
	CHECK(sessionSeconds(T, T + 3600) == 3600);
	CHECK(sessionSeconds(T, T + 12 * 3600) == 12 * 3600);
	CHECK(sessionSeconds(T, T + 12 * 3600 + 1) == 0);       // power-off, found days later
	CHECK(sessionSeconds(T, T + 93 * 3600) == 0);            // no 93-hour bogus values
	CHECK(sessionSeconds(T, T) == 0);
	CHECK(sessionSeconds(T, T - 100) == 0);                  // RTC moved back
	CHECK(sessionSeconds(0, T) == 0);                        // unset start
	CHECK(sessionSeconds(T, 100) == 0);                      // RTC reset to 1970
	CHECK(sessionSeconds(-5, T) == 0);
	char b[32];
	duration(b, sizeof b, 0); CHECK(!strcmp(b, "<1m"));
	duration(b, sizeof b, 59); CHECK(!strcmp(b, "<1m"));
	duration(b, sizeof b, 60); CHECK(!strcmp(b, "1m"));
	duration(b, sizeof b, 42 * 60); CHECK(!strcmp(b, "42m"));
	duration(b, sizeof b, 18 * 3600 + 42 * 60 + 30); CHECK(!strcmp(b, "18h 42m"));
	duration(b, sizeof b, 120 * 3600); CHECK(!strcmp(b, "120h 0m"));
	ago(b, sizeof b, T, T + 10); CHECK(!strcmp(b, "just now"));
	ago(b, sizeof b, T, T + 300); CHECK(!strcmp(b, "5m ago"));
	ago(b, sizeof b, T, T + 2 * 3600 + 5); CHECK(!strcmp(b, "2h ago"));
	ago(b, sizeof b, T, T + 3 * 86400); CHECK(!strcmp(b, "3d ago"));
	ago(b, sizeof b, T, T - 1); CHECK(b[0] == 0);
	ago(b, sizeof b, 0, T); CHECK(b[0] == 0);
	// Phase 2D wording
	playTime(b, sizeof b, 0); CHECK(!strcmp(b, "<1 min"));
	playTime(b, sizeof b, 59); CHECK(!strcmp(b, "<1 min"));
	playTime(b, sizeof b, 60); CHECK(!strcmp(b, "1 min"));
	playTime(b, sizeof b, 12 * 60 + 59); CHECK(!strcmp(b, "12 min"));
	playTime(b, sizeof b, 59 * 60 + 59); CHECK(!strcmp(b, "59 min"));
	playTime(b, sizeof b, 3600); CHECK(!strcmp(b, "1h 0m"));
	playTime(b, sizeof b, 3600 + 32 * 60 + 5); CHECK(!strcmp(b, "1h 32m"));
	playTime(b, sizeof b, 102 * 60); CHECK(!strcmp(b, "1h 42m"));
	playedTimes(b, sizeof b, 0); CHECK(!strcmp(b, "Never played"));
	playedTimes(b, sizeof b, 1); CHECK(!strcmp(b, "Played 1 time"));
	playedTimes(b, sizeof b, 30); CHECK(!strcmp(b, "Played 30 times"));
	const int64_t D = 1700000000 / 86400 * 86400 + 3600; // 01:00 of a calendar day
	lastPlayedDay(b, sizeof b, D, D + 3600); CHECK(!strcmp(b, "Today"));
	lastPlayedDay(b, sizeof b, D, D + 86400); CHECK(!strcmp(b, "Yesterday"));
	lastPlayedDay(b, sizeof b, D, D + 5 * 86400); CHECK(!strcmp(b, "5 days ago"));
	lastPlayedDay(b, sizeof b, D, D + 90 * 86400); CHECK(!strcmp(b, "3 months ago"));
	lastPlayedDay(b, sizeof b, D, D - 10); CHECK(b[0] == 0);
	lastPlayedDay(b, sizeof b, 0, D); CHECK(b[0] == 0);
	// "yesterday" is a calendar day: 23:50 and 00:10 the next day are one day apart
	lastPlayedDay(b, sizeof b, D - 3600 + 86399 - 600, D - 3600 + 86400 + 600); CHECK(!strcmp(b, "Yesterday"));
	char L[3][24];
	int n = wrapTitle("Mario Kart DS.nds", 9, 2, L); CHECK(n == 2 && !strcmp(L[0], "Mario") && !strcmp(L[1], "Kart DS"));
	n = wrapTitle("Pokemon", 9, 2, L); CHECK(n == 1 && !strcmp(L[0], "Pokemon"));
	n = wrapTitle("Supercalifragilistic", 9, 2, L); CHECK(n == 2 && !strcmp(L[0], "Supercali") && !strcmp(L[1], "fragili.."));
	n = wrapTitle("The Legend of Zelda Phantom Hourglass.nds", 9, 2, L); CHECK(n == 2 && !strcmp(L[0], "The") && !strcmp(L[1], "Legend.."));
	n = wrapTitle("", 9, 2, L); CHECK(n == 0);
	n = wrapTitle(nullptr, 9, 2, L); CHECK(n == 0);
	uint32_t s; int64_t l;
	parsePair("123,1700000000", s, l); CHECK(s == 123 && l == 1700000000);
	parsePair("77", s, l); CHECK(s == 77 && l == 0);
	parsePair("garbage", s, l); CHECK(s == 0 && l == 0);
	parsePair(nullptr, s, l); CHECK(s == 0 && l == 0);
	parsePair("99999999999999,5", s, l); CHECK(s == 0xFFFFFFF0u);
	printf(failures ? "%d FAILURES\n" : "format tests OK\n", failures);
	return failures;
}
