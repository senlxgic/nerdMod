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
	uint32_t s; int64_t l;
	parsePair("123,1700000000", s, l); CHECK(s == 123 && l == 1700000000);
	parsePair("77", s, l); CHECK(s == 77 && l == 0);
	parsePair("garbage", s, l); CHECK(s == 0 && l == 0);
	parsePair(nullptr, s, l); CHECK(s == 0 && l == 0);
	parsePair("99999999999999,5", s, l); CHECK(s == 0xFFFFFFF0u);
	printf(failures ? "%d FAILURES\n" : "format tests OK\n", failures);
	return failures;
}
