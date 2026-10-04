#include "dirIndex.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_DIRS = 24;
constexpr size_t MAX_NAMES_PER_DIR = 4096;
constexpr size_t MAX_TOTAL_NAMES = 12288;

struct DirIndex {
	std::string dir;				  // including trailing '/'
	std::vector<std::string> names; // lower-cased, sorted, unique
	bool usable = false;
};

std::vector<DirIndex> indexes;
size_t totalNames = 0;

inline char foldAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Names we are willing to answer for. Anything else gets the real lookup, so
// FAT's own name normalization can never make this index return a wrong "absent".
bool plainName(const char *s) {
	const size_t len = strlen(s);
	if (len == 0 || s[0] == '.' || s[0] == ' ' || s[len - 1] == '.' || s[len - 1] == ' ')
		return false;
	for (size_t i = 0; i < len; i++) {
		const unsigned char c = (unsigned char)s[i];
		// '~' is excluded because FAT 8.3 aliases (FOO~1.PNG) are matched by the real lookup
		if (c < 0x20 || c >= 0x7F || c == '~')
			return false;
	}
	return true;
}

DirIndex *getIndex(const std::string &dir) {
	for (DirIndex &idx : indexes) {
		if (idx.dir == dir)
			return &idx;
	}
	if (indexes.size() >= MAX_DIRS)
		return nullptr;

	indexes.emplace_back();
	DirIndex &idx = indexes.back();
	idx.dir = dir;

	errno = 0;
	DIR *pdir = opendir(dir.c_str());
	if (!pdir) {
		// A directory that does not exist has no files in it. Any other failure
		// (device not ready, I/O error...) is not proof of anything.
		idx.usable = (errno == ENOENT || errno == ENOTDIR);
		return &idx;
	}

	bool ok = true;
	while (dirent *pent = readdir(pdir)) {
		if (idx.names.size() >= MAX_NAMES_PER_DIR || totalNames + idx.names.size() >= MAX_TOTAL_NAMES) {
			ok = false;
			break;
		}
		std::string lower(pent->d_name);
		for (char &c : lower)
			c = foldAscii(c);
		idx.names.push_back(std::move(lower));
	}
	closedir(pdir);

	if (!ok) {
		idx.names.clear();
		idx.names.shrink_to_fit();
		idx.usable = false;
		return &idx;
	}

	std::sort(idx.names.begin(), idx.names.end());
	idx.names.erase(std::unique(idx.names.begin(), idx.names.end()), idx.names.end());
	totalNames += idx.names.size();
	idx.usable = true;
	return &idx;
}

} // namespace

bool pathMayExist(const char *path) {
	if (!path)
		return true;
	if (strncmp(path, "sd:/", 4) != 0 && strncmp(path, "fat:/", 5) != 0)
		return true;

	const char *slash = strrchr(path, '/');
	if (!slash || !slash[1])
		return true;
	const char *base = slash + 1;
	if (!plainName(base))
		return true;

	const DirIndex *idx = getIndex(std::string(path, slash + 1 - path));
	if (!idx || !idx->usable)
		return true;

	std::string lower(base);
	for (char &c : lower)
		c = foldAscii(c);
	return std::binary_search(idx->names.begin(), idx->names.end(), lower);
}
