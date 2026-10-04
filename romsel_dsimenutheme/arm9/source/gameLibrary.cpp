#include "gameLibrary.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fat.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nds/ndstypes.h>

#include "common/logging.h"
#include "common/systemdetails.h"
#include "common/twlmenusettings.h"
#include "fileBrowse.h"

bool nameEndsWith(const std::string_view name, const std::vector<std::string_view> extensionList);
extern void bgOperations(bool waitFrame);

namespace {

constexpr int MAX_DEPTH = 6;		 // levels below a root (the root itself is level 0)
constexpr size_t MAX_DIRS = 512;
constexpr size_t MAX_GAMES = 1024;	 // the menu itself lists at most 1024 tiles
constexpr size_t STALE_CHECK_DIRS = 24; // the shallowest folders are re-checked against the cache on every load
constexpr uint32_t CACHE_MAGIC = 0x424C4D4E; // "NMLB"
constexpr uint16_t CACHE_VERSION = 1;
constexpr size_t CACHE_MAX_BYTES = 512 * 1024;

const char *const ROOT_NAMES[] = {"roms", "games", "nds"};

// Folders that people name "$NDS", "$SNES", ... at the top of the card to keep their ROM sets together. Only these
// known names are treated as game roots (never every "$..." folder); each is probed with one stat(), nothing is listed.
const char *const DOLLAR_ROOT_NAMES[] = {"$NDS",  "$DSI", "$DSIWARE", "$GBA", "$GB",  "$GBC", "$NES", "$FDS", "$SNES", "$SFC", "$SMS", "$GG",
										 "$GEN",  "$MD",  "$A26",     "$A52", "$A78", "$COL", "$M5",  "$INT", "$MSX",  "$PCE", "$WS",  "$NGP",
										 "$SG",   "$SC",  "$PLG",     "$XEX", "$ATR"};

struct Game {
	uint32_t off;  // into pool
	uint16_t len;
	uint16_t pad;
	uint32_t size;
	uint32_t stamp; // FAT date<<16 | time of the file, opaque
};

struct Dir {
	uint32_t off;
	uint16_t len;
	uint16_t pad;
	uint32_t mtime; // from stat(), opaque
};

struct Library {
	bool loaded = false;
	std::string dev;	 // "sd:" or "fat:"
	uint32_t key = 0;	 // hash of everything the scan result depends on
	std::string pool;	 // all paths, back to back
	std::vector<Game> games;
	std::vector<Dir> dirs;
	bool truncated = false;
};

Library lib;

// ---- small helpers ---------------------------------------------------------------------------

uint32_t crcTable[256];
bool crcReady = false;

uint32_t crc32(const uint8_t *data, size_t len) {
	if (!crcReady) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			crcTable[i] = c;
		}
		crcReady = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		c = crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

uint32_t fnv(uint32_t h, const char *s) {
	while (*s)
		h = (h ^ (uint8_t)(*s++ | 0x20)) * 16777619u; // ASCII case-folded
	return (h ^ 0xFF) * 16777619u;
}

std::string trimSlashes(std::string s) {
	while (s.size() > 1 && s.back() == '/')
		s.pop_back();
	// "sd:/" keeps its slash
	return s;
}

std::string withSlash(std::string s) {
	if (s.empty() || s.back() != '/')
		s += '/';
	return s;
}

// "sd:/roms/nds/" -> dev "sd:", components {"roms","nds"}
bool splitPath(const std::string &path, std::string &dev, std::vector<std::string> &comps) {
	const size_t colon = path.find(':');
	if (colon == std::string::npos)
		return false;
	dev = path.substr(0, colon + 1);
	comps.clear();
	size_t i = colon + 1;
	while (i < path.size()) {
		while (i < path.size() && path[i] == '/')
			i++;
		size_t j = i;
		while (j < path.size() && path[j] != '/')
			j++;
		if (j > i)
			comps.emplace_back(path.substr(i, j - i));
		i = j;
	}
	return true;
}

bool isRootName(const std::string &n) {
	for (const char *r : ROOT_NAMES) {
		if (strcasecmp(n.c_str(), r) == 0)
			return true;
	}
	return false;
}

bool isDollarRootName(const std::string &n) {
	if (n.empty() || n[0] != '$')
		return false;
	for (const char *r : DOLLAR_ROOT_NAMES) {
		if (strcasecmp(n.c_str(), r) == 0)
			return true;
	}
	return false;
}

bool isDirectory(const std::string &p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The user's default start folder (if it is a real sub-folder), normalised without trailing slash.
std::string defaultFolder(const std::string &dev) {
	const int idx = (strncasecmp(dev.c_str(), "fat", 3) == 0) ? 1 : 0;
	std::string d = ms().defaultRomfolder[idx];
	if (d.empty() || d == "null")
		return "";
	d = trimSlashes(d);
	std::string ddev;
	std::vector<std::string> comps;
	if (!splitPath(d, ddev, comps) || strcasecmp(ddev.c_str(), dev.c_str()) != 0 || comps.empty())
		return ""; // the device root is not an extra root
	return d;
}

// Library roots that exist on this device, in a fixed order.
void libraryRoots(const std::string &dev, std::vector<std::string> &roots) {
	roots.clear();
	for (const char *r : ROOT_NAMES) {
		const std::string p = dev + "/" + r;
		if (isDirectory(p))
			roots.push_back(p);
	}
	for (const char *r : DOLLAR_ROOT_NAMES) {
		const std::string p = dev + "/" + r;
		if (isDirectory(p))
			roots.push_back(p);
	}
	const std::string def = defaultFolder(dev);
	if (!def.empty() && isDirectory(def)) {
		bool covered = false;
		for (const std::string &r : roots) {
			if (strcasecmp(r.c_str(), def.c_str()) == 0) {
				covered = true;
				break;
			}
		}
		if (!covered)
			roots.push_back(def);
	}
}

uint32_t scanKey(const std::string &dev, const std::vector<std::string> &roots, const std::vector<std::string_view> &exts) {
	uint32_t h = 2166136261u;
	h = fnv(h, dev.c_str());
	for (const std::string &r : roots)
		h = fnv(h, r.c_str());
	for (std::string_view e : exts)
		h = fnv(h, std::string(e).c_str());
	h = (h ^ (ms().showHidden ? 0x5A : 0xA5)) * 16777619u;
	h = (h ^ MAX_DEPTH) * 16777619u;
	return h;
}

bool skipFolderName(const char *n) {
	if (n[0] == '.' || n[0] == '_')
		return true;
	static const char *const skip[] = {"saves", "save", "ramdisks", "System Volume Information", "boxart", "covers", "icons", "banners", "cache", "cheats", "gamesettings", "screenshots", "photos", "trash", "$RECYCLE.BIN"};
	for (const char *s : skip) {
		if (strcasecmp(n, s) == 0)
			return true;
	}
	return false;
}

// Same sanity check the folder listing applies to Mega Drive ROMs.
bool validMegaDriveRom(const std::string &path) {
	if (path.size() < 4 || strcasecmp(path.c_str() + path.size() - 3, ".md") != 0)
		return true;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return true;
	u8 rev[4] = {0};
	fseek(f, 4, SEEK_SET);
	if (fread(rev, 1, 4, f) != 4) {
		fclose(f);
		return true;
	}
	const uint32_t entry = (uint32_t)rev[3] | ((uint32_t)rev[2] << 8) | ((uint32_t)rev[1] << 16) | ((uint32_t)rev[0] << 24);
	char sega[5] = {0};
	fseek(f, 0x100, SEEK_SET);
	fread(sega, 1, 4, f);
	fclose(f);
	return rev[0] == 0 && (strcmp(sega, "SEGA") == 0 || (entry >= 8 && entry < 0x3FFFFF));
}

const char *poolStr(const Library &l, uint32_t off) { return l.pool.c_str() + off; }

// ---- scanning --------------------------------------------------------------------------------

void addPath(Library &l, std::string &pool, std::vector<Game> &games, const std::string &path, uint32_t size, uint32_t stamp) {
	Game g;
	g.off = (uint32_t)pool.size();
	g.len = (uint16_t)path.size();
	g.pad = 0;
	g.size = size;
	g.stamp = stamp;
	pool.append(path);
	pool.push_back('\0');
	games.push_back(g);
	(void)l;
}

void scan(Library &out, const std::string &dev, const std::vector<std::string> &roots, const std::vector<std::string_view> &exts) {
	out.pool.clear();
	out.games.clear();
	out.dirs.clear();
	out.truncated = false;
	out.dev = dev;

	struct Q {
		std::string path;
		int depth;
	};
	std::vector<Q> queue;
	for (const std::string &r : roots)
		queue.push_back({r, 0});

	unsigned counter = 0;
	for (size_t qi = 0; qi < queue.size(); qi++) {
		const std::string dirPath = queue[qi].path; // copy: queue may grow
		const int depth = queue[qi].depth;

		if (out.dirs.size() >= MAX_DIRS) {
			out.truncated = true;
			break;
		}
		{
			Dir d;
			d.off = (uint32_t)out.pool.size();
			d.len = (uint16_t)dirPath.size();
			d.pad = 0;
			struct stat st;
			d.mtime = (stat(dirPath.c_str(), &st) == 0) ? (uint32_t)st.st_mtime : 0;
			out.pool.append(dirPath);
			out.pool.push_back('\0');
			out.dirs.push_back(d);
		}

		DIR *pdir = opendir(dirPath.c_str());
		if (!pdir)
			continue; // inaccessible folder: skip it, never fail the whole scan

		while (true) {
			if ((counter++ & 7) == 0)
				bgOperations(false);

			// Read the FAT directory entry that readdir() is about to return (same trick as the folder listing)
			uint32_t fileSize = 0, stamp = 0;
			int attrs = 0;
			static_assert(_LIBFAT_MAJOR_ == 1 && _LIBFAT_MINOR_ == 1 && _LIBFAT_PATCH_ == 5, "libfat updated! Check that this is still correct");
			{
				const u8 *state = (const u8 *)pdir->dirData->dirStruct;
				const u8 *e = state + 4; // entryData
				attrs = e[0xB];
				const uint32_t wtime = e[0x16] | (e[0x17] << 8);
				const uint32_t wdate = e[0x18] | (e[0x19] << 8);
				stamp = (wdate << 16) | wtime;
				fileSize = e[0x1C] | (e[0x1D] << 8) | (e[0x1E] << 16) | ((uint32_t)e[0x1F] << 24);
			}

			dirent *pent = readdir(pdir);
			if (!pent)
				break;
			const char *n = pent->d_name;
			if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
				continue;
			if (!ms().showHidden && ((attrs & ATTR_HIDDEN) || n[0] == '.'))
				continue;

			if (pent->d_type == DT_DIR) {
				if (depth + 1 <= MAX_DEPTH && !skipFolderName(n))
					queue.push_back({dirPath + "/" + n, depth + 1});
				continue;
			}
			if (!nameEndsWith(n, exts))
				continue;
			if (out.games.size() >= MAX_GAMES) {
				out.truncated = true;
				continue;
			}
			const std::string full = dirPath + "/" + n;
			if (!validMegaDriveRom(full))
				continue;
			addPath(out, out.pool, out.games, full, fileSize, stamp);
		}
		closedir(pdir);
	}
	logPrint("Library scan: %u games in %u folders%s\n", (unsigned)out.games.size(), (unsigned)out.dirs.size(), out.truncated ? " (truncated)" : "");
}

// ---- binary cache ----------------------------------------------------------------------------
//
// u32 magic "NMLB" | u16 version | u16 flags(bit0: truncated) | u32 key | u32 dirCount | u32 gameCount | u32 poolBytes
// dirs:  dirCount  x { u32 off, u16 len, u16 0, u32 mtime }
// games: gameCount x { u32 off, u16 len, u16 0, u32 size, u32 stamp }
// pool:  poolBytes of NUL-terminated paths
// u32 crc32 of everything above

struct Header {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint32_t key;
	uint32_t dirCount;
	uint32_t gameCount;
	uint32_t poolBytes;
};

std::string cachePath(const std::string &dev) { return dev + "/_nds/nerdMod/library.bin"; }

bool writeCache(const Library &l) {
	const std::string dir1 = l.dev + "/_nds";
	const std::string dir2 = l.dev + "/_nds/nerdMod";
	mkdir(dir1.c_str(), 0777);
	mkdir(dir2.c_str(), 0777);

	Header h;
	h.magic = CACHE_MAGIC;
	h.version = CACHE_VERSION;
	h.flags = l.truncated ? 1 : 0;
	h.key = l.key;
	h.dirCount = (uint32_t)l.dirs.size();
	h.gameCount = (uint32_t)l.games.size();
	h.poolBytes = (uint32_t)l.pool.size();

	const size_t total = sizeof(Header) + l.dirs.size() * sizeof(Dir) + l.games.size() * sizeof(Game) + l.pool.size() + 4;
	if (total > CACHE_MAX_BYTES)
		return false;
	uint8_t *buf = (uint8_t *)malloc(total);
	if (!buf)
		return false;
	uint8_t *p = buf;
	memcpy(p, &h, sizeof(h));
	p += sizeof(h);
	if (!l.dirs.empty()) {
		memcpy(p, l.dirs.data(), l.dirs.size() * sizeof(Dir));
		p += l.dirs.size() * sizeof(Dir);
	}
	if (!l.games.empty()) {
		memcpy(p, l.games.data(), l.games.size() * sizeof(Game));
		p += l.games.size() * sizeof(Game);
	}
	memcpy(p, l.pool.data(), l.pool.size());
	p += l.pool.size();
	const uint32_t crc = crc32(buf, (size_t)(p - buf));
	memcpy(p, &crc, 4);

	const std::string finalPath = cachePath(l.dev);
	const std::string tmpPath = finalPath + ".tmp";
	bool ok = false;
	FILE *f = fopen(tmpPath.c_str(), "wb");
	if (f) {
		ok = fwrite(buf, 1, total, f) == total;
		if (fclose(f) != 0)
			ok = false;
	}
	free(buf);
	if (!ok) {
		remove(tmpPath.c_str());
		return false;
	}
	remove(finalPath.c_str());
	if (rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
		remove(tmpPath.c_str());
		return false;
	}
	return true;
}

// Returns true only for a complete, consistent cache that matches `key`.
bool readCache(Library &l, const std::string &dev, uint32_t key) {
	FILE *f = fopen(cachePath(dev).c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < (long)(sizeof(Header) + 4) || (size_t)sz > CACHE_MAX_BYTES) {
		fclose(f);
		return false;
	}
	uint8_t *buf = (uint8_t *)malloc((size_t)sz);
	if (!buf) {
		fclose(f);
		return false;
	}
	const bool readOk = fread(buf, 1, (size_t)sz, f) == (size_t)sz;
	fclose(f);
	if (!readOk) {
		free(buf);
		return false;
	}

	Header h;
	memcpy(&h, buf, sizeof(h));
	bool ok = h.magic == CACHE_MAGIC && h.version == CACHE_VERSION && h.key == key && h.dirCount <= MAX_DIRS && h.gameCount <= MAX_GAMES;
	if (ok) {
		const size_t expect = sizeof(Header) + (size_t)h.dirCount * sizeof(Dir) + (size_t)h.gameCount * sizeof(Game) + h.poolBytes + 4;
		ok = expect == (size_t)sz;
	}
	if (ok) {
		uint32_t stored;
		memcpy(&stored, buf + sz - 4, 4);
		ok = crc32(buf, (size_t)sz - 4) == stored;
	}
	if (ok) {
		const uint8_t *p = buf + sizeof(Header);
		l.dirs.resize(h.dirCount);
		if (h.dirCount)
			memcpy(l.dirs.data(), p, h.dirCount * sizeof(Dir));
		p += h.dirCount * sizeof(Dir);
		l.games.resize(h.gameCount);
		if (h.gameCount)
			memcpy(l.games.data(), p, h.gameCount * sizeof(Game));
		p += h.gameCount * sizeof(Game);
		l.pool.assign((const char *)p, h.poolBytes);
		l.truncated = h.flags & 1;
		l.dev = dev;

		// every offset must point at a NUL-terminated string inside the pool
		auto good = [&](uint32_t off, uint16_t len) { return (uint64_t)off + len < l.pool.size() && l.pool[off + len] == '\0'; };
		for (const Dir &d : l.dirs)
			ok = ok && good(d.off, d.len);
		for (const Game &g : l.games)
			ok = ok && good(g.off, g.len);
	}
	free(buf);
	if (!ok) {
		l.pool.clear();
		l.games.clear();
		l.dirs.clear();
	}
	return ok;
}

// The shallowest folders come first in the scan order. If one of them changed (a game was added or
// removed there), the cache is out of date.
bool cacheStillFresh(const Library &l) {
	const size_t n = std::min(l.dirs.size(), STALE_CHECK_DIRS);
	for (size_t i = 0; i < n; i++) {
		struct stat st;
		if (stat(poolStr(l, l.dirs[i].off), &st) != 0)
			return false;
		if ((uint32_t)st.st_mtime != l.dirs[i].mtime)
			return false;
	}
	return true;
}

bool ensureLibrary(const std::string &dev, const std::vector<std::string_view> &exts) {
	std::vector<std::string> roots;
	libraryRoots(dev, roots);
	if (roots.empty()) {
		lib = Library();
		return false;
	}
	const uint32_t key = scanKey(dev, roots, exts);

	if (lib.loaded && lib.dev == dev && lib.key == key && !ms().libraryRefresh)
		return true;

	Library fresh;
	fresh.key = key;
	bool fromCache = false;
	if (!ms().libraryRefresh) {
		fromCache = readCache(fresh, dev, key) && cacheStillFresh(fresh);
		if (!fromCache) {
			fresh = Library();
			fresh.key = key;
		}
	}
	if (!fromCache) {
		scan(fresh, dev, roots, exts);
		fresh.key = key;
		if (!writeCache(fresh))
			logPrint("Library cache could not be written\n");
		if (ms().libraryRefresh) {
			ms().libraryRefresh = false;
			ms().saveSettings();
		}
	} else {
		logPrint("Library cache: %u games\n", (unsigned)fresh.games.size());
	}
	fresh.loaded = true;
	lib = std::move(fresh);
	return true;
}

} // namespace

// ---- public API ------------------------------------------------------------------------------

LibraryHomeKind gameLibraryHomeKind(const std::string &cwd) {
	std::string dev;
	std::vector<std::string> comps;
	if (!splitPath(cwd, dev, comps))
		return LIB_NOT_HOME;
	if (comps.empty())
		return LIB_HOME_MAIN; // device root
	if (comps.size() == 1 && isRootName(comps[0]))
		return LIB_HOME_MAIN;
	if (comps.size() == 1 && isDollarRootName(comps[0]))
		return LIB_HOME_FLATTEN; // a ROM-set folder such as $GBA: its sub-folders' games are listed in it too
	if (comps.size() == 2 && strcasecmp(comps[0].c_str(), "roms") == 0)
		return strcasecmp(comps[1].c_str(), "nds") == 0 ? LIB_HOME_MAIN : LIB_HOME_FLATTEN;
	const std::string def = defaultFolder(dev);
	if (!def.empty() && strcasecmp(def.c_str(), trimSlashes(cwd).c_str()) == 0)
		return LIB_HOME_MAIN;
	return LIB_NOT_HOME;
}

bool gameLibraryCollect(const std::string &cwd, const std::vector<std::string_view> &extensions, std::vector<std::string> &relPaths) {
	relPaths.clear();
	if (gameLibraryHomeKind(cwd) == LIB_NOT_HOME)
		return false;

	std::string dev;
	std::vector<std::string> comps;
	splitPath(cwd, dev, comps);
	if (!ensureLibrary(dev, extensions))
		return false;

	const std::string prefix = withSlash(trimSlashes(cwd));
	for (const Game &g : lib.games) {
		const char *p = poolStr(lib, g.off);
		if (g.len <= prefix.size() || strncasecmp(p, prefix.c_str(), prefix.size()) != 0)
			continue;
		const char *rel = p + prefix.size();
		if (!strchr(rel, '/'))
			continue; // directly in the folder: the normal listing has it
		relPaths.emplace_back(rel);
	}
	return !relPaths.empty();
}

void gameLibraryInvalidate() {
	if (lib.loaded)
		remove(cachePath(lib.dev).c_str());
	else {
		remove("sd:/_nds/nerdMod/library.bin");
		remove("fat:/_nds/nerdMod/library.bin");
	}
	lib = Library();
}

size_t gameLibraryMemoryBytes() {
	return lib.pool.capacity() + lib.games.capacity() * sizeof(Game) + lib.dirs.capacity() * sizeof(Dir);
}

// ---- home view ------------------------------------------------------------------------------

namespace {
bool browseSession = false;
}

void gameLibrarySetBrowse(bool browse) { browseSession = browse; }
bool gameLibraryBrowsing() { return browseSession; }

int gameLibraryEffectiveView() { return browseSession ? (int)TWLSettings::ELibraryFolders : (int)ms().gameLibraryView; }

std::string gameLibraryDeviceRoot(const std::string &cwd) {
	std::string dev;
	std::vector<std::string> comps;
	if (!splitPath(cwd, dev, comps))
		return "";
	return dev + "/";
}

bool gameLibraryEnterHome() {
	if (gameLibraryEffectiveView() == (int)TWLSettings::ELibraryFolders)
		return false;
	const int idx = ms().secondaryDevice;
	const std::string cur = ms().romfolder[idx];
	if (cur.empty())
		return false;
	if (gameLibraryHomeKind(cur) == LIB_NOT_HOME)
		return false; // a folder the user chose to be in: leave it
	const std::string root = gameLibraryDeviceRoot(cur);
	if (root.empty() || trimSlashes(root) == trimSlashes(cur))
		return false;
	ms().romfolder[idx] = root;
	ms().pagenum[idx] = 0;
	ms().cursorPosition[idx] = 0;
	return true;
}
