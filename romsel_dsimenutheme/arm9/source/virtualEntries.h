#pragma once
#ifndef NERDMOD_VIRTUAL_ENTRIES_H
#define NERDMOD_VIRTUAL_ENTRIES_H

// Virtual menu entries: tiles that look and behave like any other game tile but are not (only) a
// file in the folder that is being browsed.
//
//  1. Built-in apps (Camera, ...): a small static registry. Adding an app is one line in the table in
//     virtualEntries.cpp; the menu code never mentions an app by name.
//  2. Flattened games (see gameLibrary.h): games from sub-folders shown in the folder's own list.

#include <string>
#include <vector>

#include "dirEntry.h"

struct BuiltInApp {
	const char *id;			// stable identifier
	const char *title;		// fallback label (the tile normally shows the title from the app's own banner)
	const char *launchPath; // relative to <device>:/_nds/TWiLightMenu/
	const char *iconPath;	// banner/icon source relative to the same folder; nullptr = the launchPath file's own banner
	bool (*available)(const BuiltInApp &app); // nullptr = "launchPath exists"
};

int builtInAppCount();
const BuiltInApp &builtInApp(int id);
bool builtInAppAvailable(int id);
// Absolute path of the app's launch file, e.g. "sd:/_nds/TWiLightMenu/camera.srldr"
std::string builtInAppPath(int id);
// Absolute path of the file whose NDS banner provides the tile icon/title
std::string builtInAppIconPath(int id);

// True for the folders where built-in apps are shown: the device root, the library roots and the
// user's default start folder. `cwd` is what getcwd() returns.
bool builtInAppsShownIn(const std::string &cwd);

// Inserts a tile for every available built-in app at `insertAt` (the first slot after the folders).
// Returns the number of tiles inserted.
int addBuiltInEntries(std::vector<DirEntry> &entries, int insertAt);

// True only for the absolute launch/icon path of a registered built-in app. Any other .srldr file
// (system files, whatever a user drops in a folder) is NOT a game and must never get ROM/banner handling.
bool isBuiltInAppPath(const char *path);

// Last path component ("Action/Game.nds" -> "Game.nds"; names without '/' are returned as-is).
const char *entryBaseName(const std::string &name);
// Everything up to and including the last '/' ("Action/Game.nds" -> "Action/"); empty if none.
std::string entryDirPrefix(const std::string &name);

// While alive, a flattened entry behaves exactly like a normal entry of its own folder: the current
// folder is its real folder and entry->name is its plain file name. That is what all of the existing
// per-game code (settings, saves, launch checks, recently played, ...) expects.
// The saved ROM folder (ms().romfolder) follows along. The destructor puts the folder, the name and the
// saved ROM folder back, unless commitLaunch() was called.
class EntryCwdScope {
  public:
	explicit EntryCwdScope(DirEntry *entry);
	~EntryCwdScope();
	EntryCwdScope(const EntryCwdScope &) = delete;
	EntryCwdScope &operator=(const EntryCwdScope &) = delete;

	bool active() const { return switched; }
	// The game is about to be launched: stay in its real folder, make that the saved ROM folder (saves
	// and per-game files are relative to it) and remember the folder the user was browsing so the next
	// menu start returns there.
	void commitLaunch();

  private:
	DirEntry *entry;
	std::string savedCwd;
	std::string savedName;
	std::string savedRomfolder;
	bool switched = false;
	bool committed = false;
};

// Called once at menu start: after launching a flattened game the saved ROM folder is the game's real
// folder; go back to the folder the user was browsing instead.
void restoreLibraryHomeFolder();

#endif
