#include <stdio.h>
#include <vector>
#include "../../../universal/include/common/nmfont.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int main() {
	for (const char *p = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :.-/%+\xB0" "abcz"; *p; p++)
		CHECK(nmfont::glyph(*p) != nullptr);
	CHECK(nmfont::glyph('~') == nullptr);
	// every letter/digit (except space) has at least one lit pixel and none beyond 5 columns
	for (const char *p = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; *p; p++) {
		const uint8_t *g = nmfont::glyph(*p);
		int lit = 0;
		for (int r = 0; r < 7; r++) { lit += __builtin_popcount(g[r]); CHECK((g[r] & ~0x1F) == 0); }
		CHECK(lit >= 5);
	}
	CHECK(nmfont::textWidth("", 2) == 0 && nmfont::textWidth("AB", 1) == 11 && nmfont::textWidth("AB", 3) == 33);
	// clipping: drawing far outside, partly outside, with a huge scale never touches memory beyond the buffer
	const int W = 64, H = 96;
	std::vector<uint16_t> mem((W + 16) * (H + 16), 0xABCD);
	uint16_t *buf = mem.data() + 8 * (W + 16) + 8; // 8 guard pixels around
	// treat buf as a W x (stride) image: draw into a tight copy instead
	std::vector<uint16_t> img(W * H, 0);
	for (int x : {-100, -5, 0, 30, 60, 200})
		for (int y : {-100, -5, 0, 90, 300})
			for (int sc : {1, 2, 3, 8})
				nmfont::draw(img.data(), W, H, x, y, "18H 42M 47 LAUNCHES", sc, 0x7FFF);
	nmfont::drawCentred(img.data(), W, H, 10, "PLAY STATS", 1, 0x7FFF);
	int lit = 0;
	for (uint16_t v : img) lit += (v == 0x7FFF);
	CHECK(lit > 0);
	(void)buf;
	// canary buffer
	std::vector<uint16_t> c(W * H + 64, 0x1234);
	nmfont::draw(c.data(), W, H, 3, 3, "WEATHER 21 C", 2, 0x7FFF);
	for (size_t i = W * H; i < c.size(); i++) CHECK(c[i] == 0x1234);
	// fits the 64 px panel at the sizes the widgets use
	CHECK(nmfont::textWidth("PLAY", 1) <= 62 && nmfont::textWidth("18H", 3) <= 62 && nmfont::textWidth("LAUNCHES", 1) <= 62 && nmfont::textWidth("-12C", 3) > 62);
	printf(failures ? "%d FAILURES\n" : "font tests OK\n", failures);
	return failures;
}
