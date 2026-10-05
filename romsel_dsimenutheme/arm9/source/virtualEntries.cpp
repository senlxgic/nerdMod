#include "virtualEntries.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "common/systemdetails.h"
#include "common/twlmenusettings.h"
#include "gameLibrary.h"
#include "myDSiMode.h"

namespace {

std::string appFolder() { return std::string(sys().isRunFromSD() ? "sd" : "fat") + ":/_nds/TWiLightMenu/"; }

bool fileExists(const std::string &p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The DSi camera exists on DSi / DSi XL only (not on a 3DS, where the DSi cameras are not wired up) and
// the app is only offered when it is actually installed.
bool cameraAvailable(const BuiltInApp &app) {
	if (ms().kioskMode || !dsiFeatures() || ms().consoleModel >= 2)
		return false;
	return fileExists(appFolder() + app.launchPath);
}

bool photosAvailable(const BuiltInApp &app) {
	if (ms().kioskMode || !dsiFeatures())
		return false;
	return fileExists(appFolder() + app.launchPath);
}

// Weather (Wi-Fi) and Music (SD card) are DSi-family apps and are offered once their .srldr is installed.
bool dsiAppAvailable(const BuiltInApp &app) {
	if (ms().kioskMode || !dsiFeatures())
		return false;
	return fileExists(appFolder() + app.launchPath);
}

// The registry. Order = order of the tiles. A new built-in app is one more line here.
const BuiltInApp apps[] = {
	{"camera", "Camera", "camera.srldr", nullptr, cameraAvailable},
	// Photos works on any DSi-family console (it only reads picture files) and is offered once photos.srldr is installed.
	{"photos", "Photos", "photos.srldr", nullptr, photosAvailable},
	{"weather", "Weather", "weather.srldr", nullptr, dsiAppAvailable},
	{"music", "Music", "music.srldr", nullptr, dsiAppAvailable},
};

} // namespace

int builtInAppCount() { return (int)(sizeof(apps) / sizeof(apps[0])); }

const BuiltInApp &builtInApp(int id) { return apps[id]; }

bool builtInAppAvailable(int id) {
	if (id < 0 || id >= builtInAppCount())
		return false;
	const BuiltInApp &app = apps[id];
	if (app.available)
		return app.available(app);
	return fileExists(appFolder() + app.launchPath);
}

std::string builtInAppPath(int id) { return appFolder() + apps[id].launchPath; }

std::string builtInAppIconPath(int id) { return appFolder() + (apps[id].iconPath ? apps[id].iconPath : apps[id].launchPath); }

bool builtInAppsShownIn(const std::string &cwd) { return gameLibraryHomeKind(cwd) == LIB_HOME_MAIN; }

int addBuiltInEntries(std::vector<DirEntry> &entries, int insertAt) {
	if (insertAt < 0 || insertAt > (int)entries.size())
		insertAt = (int)entries.size();
	int added = 0;
	for (int id = 0; id < builtInAppCount(); id++) {
		if (!builtInAppAvailable(id))
			continue;
		DirEntry e(builtInAppIconPath(id), false, 0, false);
		e.kind = ENTRY_BUILTIN;
		e.appId = (signed char)id;
		entries.insert(entries.begin() + insertAt + added, e);
		added++;
	}
	return added;
}

bool isBuiltInAppPath(const char *path) {
	if (!path || !*path)
		return false;
	for (int id = 0; id < builtInAppCount(); id++) {
		if (builtInAppIconPath(id) == path || builtInAppPath(id) == path)
			return true;
	}
	return false;
}

const char *entryBaseName(const std::string &name) {
	const size_t slash = name.rfind('/');
	return slash == std::string::npos ? name.c_str() : name.c_str() + slash + 1;
}

std::string entryDirPrefix(const std::string &name) {
	const size_t slash = name.rfind('/');
	return slash == std::string::npos ? std::string() : name.substr(0, slash + 1);
}

EntryCwdScope::EntryCwdScope(DirEntry *e) : entry(e) {
	if (!entry || entry->kind != ENTRY_FLATTENED)
		return;
	const std::string prefix = entryDirPrefix(entry->name);
	if (prefix.empty())
		return;
	char buf[512];
	if (!getcwd(buf, sizeof(buf)))
		return;
	savedCwd = buf;
	if (chdir(prefix.c_str()) != 0)
		return; // folder is gone: leave everything as it was
	savedName = entry->name;
	entry->name = entryBaseName(savedName);
	// Code that builds save/settings paths from the saved ROM folder must see the real folder too
	savedRomfolder = ms().romfolder[ms().secondaryDevice];
	if (getcwd(buf, sizeof(buf)))
		ms().romfolder[ms().secondaryDevice] = buf;
	switched = true;
}

EntryCwdScope::~EntryCwdScope() {
	if (switched && !committed) {
		chdir(savedCwd.c_str());
		entry->name = savedName;
		ms().romfolder[ms().secondaryDevice] = savedRomfolder;
	}
}

void EntryCwdScope::commitLaunch() {
	if (!switched)
		return;
	committed = true;
	// romfolder already holds the real folder (set in the constructor); remember where the user was
	ms().libraryReturnHome[ms().secondaryDevice] = savedCwd;
}

void restoreLibraryHomeFolder() {
	const int dev = ms().secondaryDevice;
	std::string &home = ms().libraryReturnHome[dev];
	if (home.empty())
		return;
	struct stat st;
	if (stat(home.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
		ms().romfolder[dev] = home;
	home.clear();
}
