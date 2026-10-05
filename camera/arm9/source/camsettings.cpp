#include "camsettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

#include "common/systemdetails.h"
#include "fpsutil.h"
#include "videofmt.h"

namespace camsettings {

namespace {
int fps = fpsutil::DEFAULT_FPS;
int quality = vfmt::DEFAULT_QUALITY;
bool loaded = false;


std::string path() { return std::string(sys().isRunFromSD() ? "sd:" : "fat:") + "/_nds/nerdMod/camera.ini"; }
} // namespace

namespace {
void load() {
	if (loaded)
		return;
	loaded = true;
	if (!sys().fatInitOk())
		return;
	FILE *f = fopen(path().c_str(), "rb");
	if (!f)
		return;
	char buf[256];
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	fps = fpsutil::parseFps(buf);
	const char *q = strstr(buf, "VIDEO_QUALITY=");
	if (q)
		quality = vfmt::sanitizeQuality(atoi(q + 14));
}

void save() {
	loaded = true;
	if (!sys().fatInitOk())
		return;
	const std::string root = sys().isRunFromSD() ? "sd:" : "fat:";
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir((root + "/_nds/nerdMod").c_str(), 0777);
	FILE *f = fopen(path().c_str(), "wb");
	if (!f)
		return;
	fprintf(f, "[CAMERA]\nVIDEO_FPS=%d\nVIDEO_QUALITY=%d\n", fps, quality);
	fclose(f);
}
} // namespace

int videoFps() {
	load();
	return fps;
}

void setVideoFps(int value) {
	load();
	fps = fpsutil::sanitize(value);
	save();
}

int videoQuality() {
	load();
	return quality;
}

void setVideoQuality(int value) {
	load();
	quality = vfmt::sanitizeQuality(value);
	save();
}

} // namespace camsettings
