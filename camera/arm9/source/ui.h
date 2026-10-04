/*
	nerdMod Camera - user interface layer

	Top screen    : the live picture (two 256x192 RGB555 bitmap pages that the camera DMA writes into,
	                flipped without tearing) with a few small hardware-sprite overlays: viewfinder
	                brackets, Inner/Outer and Photo/Video indicators, the recording indicator and timer.
	Bottom screen : a pre-rendered 15-bit background with glossy, touch-sized buttons. The art is stored
	                as raw RGB555 in ui.bin (see tools/genassets.py); drawing a button is one rectangle copy,
	                so there is no image decoding while the camera is running. Dynamic text uses the console
	                layer on top of it.

	Nothing here touches the camera registers or its DMA.
*/
#pragma once

#include <nds.h>

#include "ui_assets.h"

// ---- setup -----------------------------------------------------------------------------------------
void uiInit();

// ---- top screen --------------------------------------------------------------------------------------
u16 *uiTopPage(int page);
void uiTopShowPage(int page);
void uiTopClear();
void uiTopFlash();	// white flash (shutter)
void uiTopFade(bool toWhite, int frames);
void uiTopOverlayVisible(bool visible);
void uiTopSetCamera(bool inner);
void uiTopSetMode(bool video);
void uiTopSetRecording(bool recording, u32 seconds);	// recording indicator + timer (call once per frame)

// ---- bottom screen -----------------------------------------------------------------------------------
struct UiButton {
	int id;
	short x, y, w, h;
	int normal;		// UiId
	int pressed;	// UiId, or -1 for "no pressed look"
	bool enabled;
};

void uiBottomDrawBackground();
void uiBottomImage(int uiId, int x, int y);
void uiBottomRestore(int x, int y, int w, int h);	// copy the background back
void uiShowButtons(const UiButton *buttons, int count);	// draw all, make them touchable
void uiSetButtonImages(int id, int normal, int pressed, bool enabled = true);
void uiPressFeedback(int id, int frames = 5);	// pressed look for a physical key
int uiHandleInput(u32 down, u32 up);	// returns the id of an activated button, or -1; call once per frame
void uiTick();	// advances timed visuals; call once per frame
void uiClearButtons();

// ---- text on the bottom screen (console layer) -------------------------------------------------------
void uiTextClear();
void uiTextAt(int col, int row, const char *text);
void uiTextCentred(int row, const char *text);
// The status plate on the bottom screen (one line, ~19 characters)
void uiStatus(const char *text);
// Text in the title bar, right side (e.g. "3 / 12")
void uiBarText(const char *text);

// ---- dialogs -----------------------------------------------------------------------------------------
void uiDrawDialogPanel();
