#include "store.h"

#include <dirent.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store {

namespace {
std::string rootPath;

bool dirExists(const char *p) {
	DIR *d = opendir(p);
	if (!d)
		return false;
	closedir(d);
	return true;
}

void mkdirs(const std::string &path) {
	// path is a file path: create every folder above it
	for (size_t i = 1; i < path.size(); i++)
		if (path[i] == '/') {
			const std::string sub = path.substr(0, i);
			if (sub.size() > 4) // skip "sd:" / "fat:/" roots
				mkdir(sub.c_str(), 0777);
		}
}
} // namespace

bool init() {
	if (!fatInitDefault())
		return false;
	if (dirExists("sd:/"))
		rootPath = "sd:";
	else if (dirExists("fat:/"))
		rootPath = "fat:";
	else
		return false;
	return true;
}

const std::string &root() { return rootPath; }
std::string settingsPath() { return rootPath + "/_nds/nerdMod/weather.ini"; }
std::string cachePath() { return rootPath + "/_nds/nerdMod/cache/weather/weather.ini"; }

bool readFile(const std::string &path, std::string &out, size_t maxBytes) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	char buf[512];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		if (out.size() + n > maxBytes) {
			fclose(f);
			return false;
		}
		out.append(buf, n);
	}
	fclose(f);
	return true;
}

bool writeFile(const std::string &path, const char *data, size_t len) {
	mkdirs(path);
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data, 1, len, f) == len;
	if (fclose(f) != 0 || !ok) {
		remove(tmp.c_str());
		return false;
	}
	remove(path.c_str());
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		remove(tmp.c_str());
		return false;
	}
	return true;
}

} // namespace store
