#include "camsettings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

#include "common/systemdetails.h"
#include "fpsutil.h"

namespace camsettings {

namespace {
int fps = fpsutil::DEFAULT_FPS;
bool loaded = false;

std::string path() { return std::string(sys().isRunFromSD() ? "sd:" : "fat:") + "/_nds/nerdMod/camera.ini"; }
} // namespace

int videoFps() {
	if (!loaded) {
		loaded = true;
		if (sys().fatInitOk()) {
			FILE *f = fopen(path().c_str(), "rb");
			if (f) {
				char buf[256];
				const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
				buf[n] = 0;
				fclose(f);
				fps = fpsutil::parseFps(buf);
			}
		}
	}
	return fps;
}

void setVideoFps(int value) {
	fps = fpsutil::sanitize(value);
	loaded = true;
	if (!sys().fatInitOk())
		return;
	const std::string root = sys().isRunFromSD() ? "sd:" : "fat:";
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir((root + "/_nds/nerdMod").c_str(), 0777);
	FILE *f = fopen(path().c_str(), "wb");
	if (!f)
		return;
	fprintf(f, "[CAMERA]\nVIDEO_FPS=%d\n", fps);
	fclose(f);
}

} // namespace camsettings
