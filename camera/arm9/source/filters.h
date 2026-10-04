#pragma once
#ifndef NERDMOD_CAMERA_FILTERS_H
#define NERDMOD_CAMERA_FILTERS_H

// nerdMod Camera effects. Original, small, integer-only implementations (no convolution, no floating point):
// every colour effect is a pure function of one RGB pixel, so the live preview and the video use a 32K-entry
// lookup table built once when the effect is chosen. MIRROR copies the left half of each row, flipped, onto the
// right half.

#include <nds.h>

namespace fx {

enum Effect : int {
	NORMAL = 0,
	MONO,
	SEPIA,
	NEGATIVE,
	COOL,
	WARM,
	POSTERIZE,
	CONTRAST,
	MIRROR,
	COUNT,
};

const char *name(Effect e);
// Status-bar text for an effect other than NORMAL ("Effect: SEPIA"); nullptr for NORMAL.
const char *statusLabel(Effect e);
// Video is limited to the four cheapest effects so the 10 fps recorder keeps its time budget.
bool videoSupported(Effect e);

Effect current();
// Selects an effect (builds / frees the lookup table). Returns false (and stays on NORMAL) if memory is short.
bool set(Effect e);
// Next/previous effect; when `videoOnly` the unsupported ones are skipped.
Effect step(Effect from, int dir, bool videoOnly);

// In place on a 256x192 RGB555 frame (VRAM page or RAM slot). A no-op for NORMAL.
void applyFrame(u16 *frame);

// One 8-bit pixel (photos). Not for MIRROR (the photo writer mirrors the columns itself).
void applyRgb8(u8 &r, u8 &g, u8 &b);

} // namespace fx

#endif
