#include "nmdiag.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace nmdiag {
namespace {
std::string log;
bool lastFailed = false;
std::string lastPath;
int notice = 0;

bool ensureDir(const std::string &dir) {
	struct stat st;
	if (stat(dir.c_str(), &st) == 0)
		return S_ISDIR(st.st_mode);
	return mkdir(dir.c_str(), 0777) == 0 || errno == EEXIST;
}
} // namespace

void begin(const char *title) {
	log = title;
	log += "\n";
	lastFailed = false;
}

void add(const char *fmt, ...) {
	if (log.size() > 6000)
		return; // bounded
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	log += line;
}

bool flush() {
	static const char *const devices[] = {"sd:", "fat:"};
	std::string errors;
	for (const char *dev : devices) {
		const std::string root = std::string(dev) + "/_nds";
		const std::string dir = root + "/nerdMod";
		if (!ensureDir(root)) {
			char b[96];
			snprintf(b, sizeof(b), "[mkdir %s failed errno=%d]\n", root.c_str(), errno);
			errors += b;
			continue;
		}
		if (!ensureDir(dir)) {
			char b[96];
			snprintf(b, sizeof(b), "[mkdir %s failed errno=%d]\n", dir.c_str(), errno);
			errors += b;
			continue;
		}
		const std::string path = dir + "/photo-status.txt";
		FILE *f = fopen(path.c_str(), "wb");
		if (!f) {
			char b[128];
			snprintf(b, sizeof(b), "[fopen %s failed errno=%d]\n", path.c_str(), errno);
			errors += b;
			continue;
		}
		const std::string out = log + errors;
		const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
		fclose(f);
		if (ok) {
			lastPath = path;
			lastFailed = false;
			return true;
		}
	}
	// last resort: the card root, so that a user can at least find the reason
	FILE *f = fopen("sd:/nerdmod-photo-status.txt", "wb");
	if (f) {
		const std::string out = log + errors;
		fwrite(out.data(), 1, out.size(), f);
		fclose(f);
		lastPath = "sd:/nerdmod-photo-status.txt";
		lastFailed = false;
		return true;
	}
	lastPath.clear();
	if (!lastFailed)
		notice = 240;
	lastFailed = true;
	return false;
}

bool failed() { return lastFailed; }
const char *where() { return lastPath.c_str(); }
int noticeFramesLeft() { return notice; }
void noticeTick() {
	if (notice > 0)
		notice--;
}

} // namespace nmdiag
