#pragma once
#ifndef NERDMOD_DIR_ENTRY_H
#define NERDMOD_DIR_ENTRY_H

#include <string>

// What a menu tile stands for.
//  ENTRY_NORMAL    - a file or folder in the folder being browsed (the stock behaviour).
//  ENTRY_FLATTENED - a game that lives in a sub-folder of the folder being browsed. `name` is its
//                    path relative to the current folder ("Action/Game.nds"), so every fopen()/stat()
//                    that is relative to the current folder keeps working. It is launched through the
//                    normal launch pipeline from its real folder (see EntryCwdScope).
//  ENTRY_BUILTIN   - an app that ships with nerdMod (see BuiltInApp). `name` is the absolute path of
//                    the app's .srldr, whose own NDS banner provides the tile icon and title.
enum EntryKind : unsigned char {
	ENTRY_NORMAL = 0,
	ENTRY_FLATTENED = 1,
	ENTRY_BUILTIN = 2,
};

struct DirEntry {
	DirEntry(std::string name, bool isDirectory, int position, int customPos) : name(name), isDirectory(isDirectory), position(position), customPos(customPos) {}
	DirEntry() {}

	std::string name;
	bool isDirectory = false;
	int position = 0;
	bool customPos = false;
	EntryKind kind = ENTRY_NORMAL;
	signed char appId = -1; // index into the BuiltInApp registry for ENTRY_BUILTIN
};

#endif
