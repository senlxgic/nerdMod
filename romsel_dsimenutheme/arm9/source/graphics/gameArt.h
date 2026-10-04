#pragma once
#ifndef NERDMOD_GAME_ART_H
#define NERDMOD_GAME_ART_H

// Selected-game artwork in the LEFT lane of the top screen's photo frame. The user's photo (centre) is never
// replaced: the panel is a small overlay that is removed again by copying the photo's own pixels back.
//
// Sources, in order: the game's box art (existing boxart/ folder), the banner icon enlarged, a generated
// placeholder. Everything is local; nothing is downloaded. At most four rendered panels (12 KB each) are cached.

#include <string>

namespace gameArt {

constexpr int PANEL_X = 27;
constexpr int PANEL_Y = 54;
constexpr int PANEL_W = 64;
constexpr int PANEL_H = 96;

// DSi theme with the photo frame on (the only layout that has a photo to protect). Otherwise the existing
// centred box art is used unchanged.
bool enabled();
bool visible();

// Removes the panel and puts the photo (or black) back underneath. Cheap no-op if nothing is shown.
void clear();
// The photo frame was redrawn from scratch: forget the panel without touching the screen.
void forget();

// Optional two-line caption (e.g. "12 PLAYS" / "3H 21M") drawn over the bottom of the NEXT panel shown; empty = none.
void setCaption(const char *line1, const char *line2);

// Each returns true if a panel is now visible. `key` identifies the tile (an unchanged key is not redrawn).
bool showBoxArtFile(const std::string &key, const char *pngPath);
bool showIcon(const std::string &key, const char *romPath, const char *label);
bool showPlaceholder(const std::string &key, const char *label);

} // namespace gameArt

#endif
