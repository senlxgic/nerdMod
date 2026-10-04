#pragma once
#ifndef _DIRINDEX_H_
#define _DIRINDEX_H_

// Session-level "does this file possibly exist?" index for files on the SD
// card / flashcard (paths starting with "sd:/" or "fat:/").
//
// A failed access()/fopen() on FAT has to walk every directory in the path, so
// the many lookups that miss (custom icons, box art, theme texture extensions)
// are surprisingly expensive. This lists each parent directory ONCE per
// session and answers "definitely absent" from memory.
//
// Contract:
//  - Returns false ONLY when the file is known not to exist. The caller must
//    still open/access the file when this returns true, so a stale "present"
//    is harmless.
//  - Falls back to returning true (i.e. "go ahead and do the real lookup") for
//    non-FAT paths (nitro:/), unusual names (non-ASCII, leading '.'/' ',
//    trailing '.'/' '), directories that could not be listed, and when the
//    bounded cache is full.
//  - Name matching is ASCII case-insensitive, like FAT.
//  - Only use it for directories that nothing writes to while the menu is
//    running (icons, boxart, themes, fonts). It is NOT invalidated within a
//    session: the menu process is restarted on every game launch/return.
//  - Bounded: at most 24 directories, 4096 names per directory, 12288 total.
bool pathMayExist(const char *path);

#endif
