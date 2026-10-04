#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "ui_bin.h"	// generated from assets/ui.bin by the build (bin2o)

namespace {

const u16 *const art = (const u16 *)ui_bin;

inline const u16 *imagePixels(int id) { return art + UI_IMAGES[id].offset; }

// ---- screens ---------------------------------------------------------------------------------------
u16 *const topPages[2] = {(u16 *)0x06000000, (u16 *)0x06020000}; // VRAM A, VRAM B (main BG bitmaps)
u16 *bottomGfx = nullptr;										 // sub BG3 bitmap
PrintConsole subConsole;

// ---- sprites ---------------------------------------------------------------------------------------
enum { SPR_BR_TL = 0, SPR_BR_TR, SPR_BR_BL, SPR_BR_BR, SPR_CAM, SPR_MODE, SPR_RECPLATE, SPR_RECDOT };

u16 *gfxBracket = nullptr, *gfxInner = nullptr, *gfxOuter = nullptr, *gfxPhoto = nullptr, *gfxVideo = nullptr;
u16 *gfxRecPlate = nullptr, *gfxRecDot = nullptr;
u16 recPlateBuf[64 * 32];

bool overlayVisible = true;
bool camInner = false;
bool modeVideo = false;
bool recActive = false;
u32 recSeconds = 0xFFFFFFFF;
int blinkFrames = 0;

void copyToGfx(u16 *dst, const u16 *src, int w, int h) { dmaCopyHalfWords(3, src, dst, w * h * 2); }

void putSprite(int id, int x, int y, u16 *gfx, SpriteSize size, bool hide, bool hflip = false, bool vflip = false) {
	oamSet(&oamMain, id, x, y, 0, 15, size, SpriteColorFormat_Bmp, gfx, -1, false, hide || !overlayVisible, hflip, vflip, false);
}

void updateSprites() {
	putSprite(SPR_BR_TL, 4, 4, gfxBracket, SpriteSize_32x32, false, false, false);
	putSprite(SPR_BR_TR, 256 - 32 - 4, 4, gfxBracket, SpriteSize_32x32, false, true, false);
	putSprite(SPR_BR_BL, 4, 192 - 32 - 4, gfxBracket, SpriteSize_32x32, false, false, true);
	putSprite(SPR_BR_BR, 256 - 32 - 4, 192 - 32 - 4, gfxBracket, SpriteSize_32x32, false, true, true);
	putSprite(SPR_CAM, 12, 12, camInner ? gfxInner : gfxOuter, SpriteSize_64x32, false);
	putSprite(SPR_MODE, 256 - 64 - 12, 12, modeVideo ? gfxVideo : gfxPhoto, SpriteSize_64x32, false);
	putSprite(SPR_RECPLATE, 96, 12, gfxRecPlate, SpriteSize_64x32, !recActive);
	const bool dotOn = recActive && ((blinkFrames / 30) % 2 == 0);
	putSprite(SPR_RECDOT, 97, 12, gfxRecDot, SpriteSize_16x16, !dotOn);
	oamUpdate(&oamMain);
}

void drawRecTimer(u32 seconds) {
	// plate background, then the digits "MM:SS" (the dot sprite sits on the left)
	memcpy(recPlateBuf, imagePixels(UI_SPR_RECPLATE), sizeof(recPlateBuf));
	const u16 *digits = imagePixels(UI_DIGITS);
	const int atlasW = UI_IMAGES[UI_DIGITS].w;
	const u32 minutes = (seconds / 60) % 100;
	const u32 secs = seconds % 60;
	const int glyphs[5] = {(int)(minutes / 10), (int)(minutes % 10), 10, (int)(secs / 10), (int)(secs % 10)};
	int x = 22;
	for (int g = 0; g < 5; g++) {
		for (int row = 0; row < 12; row++) {
			for (int col = 0; col < 8; col++) {
				const u16 px = digits[row * atlasW + glyphs[g] * 8 + col];
				if (px & 0x8000)
					recPlateBuf[(2 + row) * 64 + x + col] = px;
			}
		}
		x += (glyphs[g] == 10) ? 6 : 8;	// the colon sits closer
	}
	DC_FlushRange(recPlateBuf, sizeof(recPlateBuf)); // DMA reads RAM, not the cache
	copyToGfx(gfxRecPlate, recPlateBuf, 64, 32);
}

// ---- bottom buttons --------------------------------------------------------------------------------
constexpr int MAX_BUTTONS = 12;
UiButton buttons[MAX_BUTTONS];
int buttonCount = 0;
int pressedIndex = -1;	// button the finger is on
bool fingerInside = false;
int feedbackIndex = -1;
int feedbackFrames = 0;
touchPosition lastTouch;

int findIndex(int id) {
	for (int i = 0; i < buttonCount; i++)
		if (buttons[i].id == id)
			return i;
	return -1;
}

void drawButton(int i, bool pressed) {
	const UiButton &b = buttons[i];
	const int img = (pressed && b.pressed >= 0) ? b.pressed : b.normal;
	uiBottomImage(img, b.x, b.y);
}

bool inside(const UiButton &b, int px, int py) { return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h; }

} // namespace

// =================================================================================================
void uiInit() {
	// the art lives in the (cached) main RAM image; make sure the CPU view and RAM agree before DMA reads it
	DC_FlushRange(ui_bin, ui_bin_size);

	videoSetMode(MODE_5_2D);
	videoSetModeSub(MODE_5_2D);
	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
	vramSetBankC(VRAM_C_SUB_BG_0x06200000);
	vramSetBankE(VRAM_E_MAIN_SPRITE);

	// top: two bitmap pages for the camera picture
	bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	bgSetMapBase(3, 0);
	uiTopClear();

	// bottom: pre-rendered bitmap at 0x8000 in the sub BG VRAM, console text on BG0 in the first 32 KB
	const int sub3 = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 2, 0);
	bottomGfx = (u16 *)bgGetGfxPtr(sub3);
	consoleInit(&subConsole, 0, BgType_Text4bpp, BgSize_T_256x256, 8, 0, false, true);
	BG_PALETTE_SUB[0] = RGB15(26, 29, 31);
	BG_PALETTE_SUB[255] = RGB15(1, 6, 14); // dark blue text
	iprintf("\x1b[2J");
	uiBottomDrawBackground();

	// sprites (main screen only)
	oamInit(&oamMain, SpriteMapping_Bmp_1D_128, false);
	REG_DISPCNT |= DISPLAY_SPR_ACTIVE;
	gfxBracket = oamAllocateGfx(&oamMain, SpriteSize_32x32, SpriteColorFormat_Bmp);
	gfxInner = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_Bmp);
	gfxOuter = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_Bmp);
	gfxPhoto = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_Bmp);
	gfxVideo = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_Bmp);
	gfxRecPlate = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_Bmp);
	gfxRecDot = oamAllocateGfx(&oamMain, SpriteSize_16x16, SpriteColorFormat_Bmp);
	copyToGfx(gfxBracket, imagePixels(UI_SPR_BRACKET), 32, 32);
	copyToGfx(gfxInner, imagePixels(UI_SPR_INNER), 64, 32);
	copyToGfx(gfxOuter, imagePixels(UI_SPR_OUTER), 64, 32);
	copyToGfx(gfxPhoto, imagePixels(UI_SPR_PHOTO), 64, 32);
	copyToGfx(gfxVideo, imagePixels(UI_SPR_VIDEO), 64, 32);
	copyToGfx(gfxRecDot, imagePixels(UI_SPR_RECDOT), 16, 16);
	drawRecTimer(0);
	updateSprites();

	setBrightness(3, 0);
}

// ---- top ---------------------------------------------------------------------------------------------
u16 *uiTopPage(int page) { return topPages[page & 1]; }
void uiTopShowPage(int page) { bgSetMapBase(3, (page & 1) ? 8 : 0); } // 8 * 16 KB = the second 128 KB bitmap

void uiTopClear() {
	for (int i = 0; i < 2; i++)
		dmaFillHalfWords(0x8000, topPages[i], 256 * 256 * 2);
}

void uiTopFlash() {
	setBrightness(1, 16);
	for (int i = 0; i < 3; i++)
		swiWaitForVBlank();
	for (int level = 16; level >= 0; level -= 4) {
		setBrightness(1, level);
		swiWaitForVBlank();
	}
}

void uiTopFade(bool toWhite, int frames) {
	if (frames < 1)
		frames = 1;
	for (int i = 1; i <= frames; i++) {
		const int level = toWhite ? (16 * i / frames) : (16 - 16 * i / frames);
		setBrightness(1, level);
		swiWaitForVBlank();
	}
}

void uiTopOverlayVisible(bool visible) {
	overlayVisible = visible;
	updateSprites();
}

void uiTopSetCamera(bool inner) {
	camInner = inner;
	updateSprites();
}

void uiTopSetMode(bool video) {
	modeVideo = video;
	updateSprites();
}

void uiTopSetRecording(bool recording, u32 seconds) {
	if (recording != recActive) {
		recActive = recording;
		blinkFrames = 0;
		recSeconds = 0xFFFFFFFF;
	}
	if (recording) {
		blinkFrames++;
		if (seconds != recSeconds) {
			recSeconds = seconds;
			drawRecTimer(seconds);
		}
	}
	updateSprites();
}

// ---- bottom ------------------------------------------------------------------------------------------
void uiBottomDrawBackground() {
	dmaCopyHalfWords(3, imagePixels(UI_BG), bottomGfx, 256 * 192 * 2);
}

void uiBottomImage(int uiId, int x, int y) {
	const UiImage &im = UI_IMAGES[uiId];
	const u16 *src = imagePixels(uiId);
	for (int row = 0; row < im.h; row++)
		dmaCopyHalfWords(3, src + row * im.w, bottomGfx + (y + row) * 256 + x, im.w * 2);
}

void uiBottomRestore(int x, int y, int w, int h) {
	const u16 *bg = imagePixels(UI_BG);
	for (int row = 0; row < h; row++)
		dmaCopyHalfWords(3, bg + (y + row) * 256 + x, bottomGfx + (y + row) * 256 + x, w * 2);
}

void uiClearButtons() {
	buttonCount = 0;
	pressedIndex = -1;
	feedbackIndex = -1;
}

void uiShowButtons(const UiButton *list, int count) {
	buttonCount = count > MAX_BUTTONS ? MAX_BUTTONS : count;
	for (int i = 0; i < buttonCount; i++)
		buttons[i] = list[i];
	pressedIndex = -1;
	feedbackIndex = -1;
	for (int i = 0; i < buttonCount; i++)
		drawButton(i, false);
}

void uiSetButtonImages(int id, int normal, int pressed, bool enabled) {
	const int i = findIndex(id);
	if (i < 0)
		return;
	buttons[i].normal = normal;
	buttons[i].pressed = pressed;
	buttons[i].enabled = enabled;
	drawButton(i, pressedIndex == i && fingerInside);
}

void uiPressFeedback(int id, int frames) {
	const int i = findIndex(id);
	if (i < 0)
		return;
	feedbackIndex = i;
	feedbackFrames = frames;
	drawButton(i, true);
}

void uiTick() {
	if (feedbackIndex >= 0 && --feedbackFrames <= 0) {
		const int i = feedbackIndex;
		feedbackIndex = -1;
		if (i < buttonCount)
			drawButton(i, false);
	}
}

int uiHandleInput(u32 down, u32 up) {
	int result = -1;
	touchPosition t;
	if (keysHeld() & KEY_TOUCH) {
		touchRead(&t);
		lastTouch = t;
	}

	if (down & KEY_TOUCH) {
		for (int i = 0; i < buttonCount; i++) {
			if (buttons[i].enabled && inside(buttons[i], lastTouch.px, lastTouch.py)) {
				pressedIndex = i;
				fingerInside = true;
				drawButton(i, true);
				break;
			}
		}
	} else if (pressedIndex >= 0 && (keysHeld() & KEY_TOUCH)) {
		const bool now = inside(buttons[pressedIndex], lastTouch.px, lastTouch.py);
		if (now != fingerInside) {
			fingerInside = now;
			drawButton(pressedIndex, now);
		}
	}

	if ((up & KEY_TOUCH) && pressedIndex >= 0) {
		const int i = pressedIndex;
		pressedIndex = -1;
		drawButton(i, false);
		if (fingerInside)
			result = buttons[i].id;
		fingerInside = false;
	}
	return result;
}

// ---- text --------------------------------------------------------------------------------------------
void uiTextClear() { iprintf("\x1b[2J"); }

void uiTextAt(int col, int row, const char *text) { iprintf("\x1b[%d;%dH%s", row, col, text); }

void uiTextCentred(int row, const char *text) {
	const int len = (int)strlen(text);
	iprintf("\x1b[%d;0H%32s", row, "");
	uiTextAt(len >= 32 ? 0 : (32 - len) / 2, row, text);
}

void uiStatus(const char *text) {
	// plate: x 92..248, y 158..184; 18 characters from column 12 (x 96) in row 21 (y 168..176)
	char line[24];
	snprintf(line, sizeof(line), "%-18.18s", text ? text : "");
	uiTextAt(12, 21, line);
}

void uiBarText(const char *text) {
	char line[24];
	snprintf(line, sizeof(line), "%19.19s", text ? text : "");
	uiTextAt(13, 1, line);
}

void uiDrawDialogPanel() {
	uiBottomImage(UI_DIALOG, UI_RECT_DIALOG);
}
