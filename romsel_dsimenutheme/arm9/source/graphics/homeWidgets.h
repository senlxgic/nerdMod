#pragma once
#ifndef NERDMOD_HOME_WIDGETS_H
#define NERDMOD_HOME_WIDGETS_H

// nerdMod home screen widgets on the top screen, as small cards laid over the edges of the photo frame:
//   LEFT   Play Stats (replaced by the selected game's artwork, see gameArt.h, while a game is selected)
//   CENTRE the user's photo - never covered, never replaced
//   RIGHT  Weather (cached values, shown immediately; updated in the background, never blocking)
// Cards are removed by copying the photo's own pixels back, like gameArt. Only the DSi theme with the photo frame on has
// this layout; everywhere else nothing here does anything.

namespace homeWidgets {

constexpr int CARD_W = 54;
constexpr int CARD_H = 84;
constexpr int LEFT_X = 27, RIGHT_X = 175, CARD_Y = 60;

// The top screen frame was repainted from scratch: the cards are gone and must be drawn again.
void invalidate();
// Once per menu frame, after the other top-screen work. Cheap when nothing changed; draws at most one card per call.
void tick();
// gameArt removed its panel from the left lane: draw the Play Stats card again.
void artCleared();

} // namespace homeWidgets

#endif
