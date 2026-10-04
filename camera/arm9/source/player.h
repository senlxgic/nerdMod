// Video playback for the album (video only; the audio track is stored in the file but is not played on the DSi).
#pragma once

#include <string>

enum class PlayerExit { Back, PowerExit };

// Plays a .nvid file full screen on the top screen with transport buttons on the bottom screen.
// A / PLAY: play or pause   Left / Right (or the arrow buttons): seek -/+ 5 s   B / BACK: back
PlayerExit playerRun(const std::string &path, const std::string &title);
