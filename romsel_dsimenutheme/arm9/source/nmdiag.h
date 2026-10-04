#pragma once
#ifndef NERDMOD_DIAG_H
#define NERDMOD_DIAG_H

// Small, always-on diagnostic log for the home screen: <device>:/_nds/nerdMod/photo-status.txt.
// The log is kept in memory and rewritten completely by flush(). flush() creates the folders, tries the
// SD card first (explicit "sd:" path), then the flashcard ("fat:"), then the card root, and records errno
// for every failing step. It never throws and never blocks on anything but the file system.
namespace nmdiag {

void begin(const char *title); // clears the log
void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool flush();     // true if the file was written somewhere
bool failed();    // the last flush() could not write anywhere
const char *where(); // path that was written ("" if none)

// Temporary on-screen note (frames left); the menu shows "Photo diagnostic write failed" while > 0.
int noticeFramesLeft();
void noticeTick();

} // namespace nmdiag

#endif
