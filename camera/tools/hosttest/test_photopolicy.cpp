#include <stdio.h>
#include <string.h>
#include "../../../universal/include/common/nmphoto.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int main() {
	using namespace nmphoto;
	// the owner's case: nerdMod ON + theme RenderPhoto=0 must render
	Decision d = decide(false, true, false, true);
	CHECK(d.show && d.overridden && !strcmp(d.result, "DISPLAYED_BY_NERDMOD_OVERRIDE"));
	// nerdMod OFF always wins
	CHECK(!decide(false, false, true, true).show);
	CHECK(!decide(false, false, false, true).show);
	// normal theme frame
	d = decide(false, true, true, true);
	CHECK(d.show && !d.overridden && !strcmp(d.result, "DISPLAYED"));
	// macro mode and themes without a photo area
	CHECK(!decide(true, true, true, true).show);
	CHECK(!decide(false, true, false, false).show);
	CHECK(!strcmp(decide(false, true, false, false).result, "SKIPPED_THEME_HAS_NO_PHOTO_AREA"));
	if (!failures) printf("test_photopolicy: all passed\n");
	return failures ? 1 : 0;
}
