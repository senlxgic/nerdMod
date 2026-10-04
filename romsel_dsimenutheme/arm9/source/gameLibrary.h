#pragma once
#ifndef NERDMOD_GAME_LIBRARY_H
#define NERDMOD_GAME_LIBRARY_H

// Game library: lets the DSi menu show the games that live in sub-folders of a "home" folder as if they
// were in that folder ("All Games" / "Mixed" views, see TWLSettings::gameLibraryView).
//
// Nothing is moved, copied or linked: a flattened tile carries the game's real path relative to the
// folder being browsed and is launched from its real folder by the normal launch pipeline.
//
// Scope and limits (all bounded on purpose):
//  - Only the library roots are scanned: <device>:/roms, /games, /nds (when they exist) and the user's
//    default start folder. The rest of the card is never walked.
//  - At most 6 folder levels, 512 folders and 1024 games. Folders starting with '.' or '_' and the
//    saves/ramdisks/boxart/icons/cache/... folders are skipped, as are hidden entries (unless the menu is
//    set to show them) and files whose extension the menu would not list anyway.
//  - The result is cached in <device>:/_nds/nerdMod/library.bin (compact binary, CRC protected). A bad
//    or outdated cache is simply rebuilt; a missing game can never come from a cache problem because the
//    cache is only used when it matches the current settings and the shallowest folders are unchanged.
//    "Refresh Library" in the settings forces a rebuild.

#include <string>
#include <string_view>
#include <vector>

enum LibraryHomeKind {
	LIB_NOT_HOME = 0,
	LIB_HOME_FLATTEN = 1, // flattened games are shown here (e.g. /roms/gba)
	LIB_HOME_MAIN = 2,	  // device root, library roots, /roms/nds, the default start folder: games and built-in apps
};

// Classifies the folder the menu is browsing (`cwd` as returned by getcwd()).
LibraryHomeKind gameLibraryHomeKind(const std::string &cwd);

// Appends the games below `cwd` (at least one folder deep, so nothing is listed twice) to `relPaths`, as
// paths relative to `cwd` with '/' separators. Returns false when `cwd` is not a library home or no game
// was found; in that case `relPaths` is left empty and the caller should fall back to the plain folder view.
bool gameLibraryCollect(const std::string &cwd, const std::vector<std::string_view> &extensions, std::vector<std::string> &relPaths);

// Forget the cached library (in memory and on disk); the next collect rebuilds it.
void gameLibraryInvalidate();

// Rough memory in use by the in-memory library, for the on-screen/log statistics.
size_t gameLibraryMemoryBytes();

// The library "home" for the current session. The home view (settings) shows games from the ROM roots
// directly; "Browse Folders" (tile menu) switches this session back to plain folders without touching the setting.
void gameLibrarySetBrowse(bool browse);
bool gameLibraryBrowsing();
// TWLSettings::TLibraryView value that applies right now (Folders while browsing).
int gameLibraryEffectiveView();
// "sd:/" for any path on the SD card, "fat:/" for the flashcard; empty if `cwd` has no device.
std::string gameLibraryDeviceRoot(const std::string &cwd);
// At menu start in a library view: when the saved folder is a library root / ROM-set folder (not a folder
// the user went to on purpose), go to the device root, where all roots are combined. Returns true if it moved.
bool gameLibraryEnterHome();

#endif
