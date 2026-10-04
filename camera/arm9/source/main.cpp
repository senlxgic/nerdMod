/*
	nerdMod Camera

	A small Nintendo DSi camera application for nerdMod / TWiLight Menu++:
	live preview from the inner or outer camera, photo capture to the SD card,
	and a simple album. Launched like any other app from the menu; it returns
	to the menu it was started from.

	Controls (touch buttons mirror them):
	  Camera:  A shutter | X or L/R switch camera | Y album | B back
	  Album:   Left/Right or L/R browse | X delete | B back
*/

#include <nds.h>

#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "camera.h"
#include "photos.h"

#include "common/nds_loader_arm9.h"
#include "common/systemdetails.h"
#include "common/twlmenusettings.h"
#include "myDSiMode.h"

// Symbols the shared TWiLight sources expect from the application
bool fadeType = false;
bool controlTopBright = true;
bool controlBottomBright = true;
bool useTwlCfg = false;

namespace {

constexpr size_t MAX_ALBUM_ENTRIES = 512;

u16 *const previewPage[2] = {(u16 *)0x06000000, (u16 *)0x06020000}; // VRAM A, VRAM B (main BG bitmaps)

PrintConsole subConsole;
bool exitRequested = false;

//---------------------------------------------------------------- text / buttons

struct Button {
	int x, y, w, h;
	const char *line1;
	const char *line2;
};

void at(int col, int row, const char *text) { iprintf("\x1b[%d;%dH%s", row, col, text); }

void clearRow(int row) { iprintf("\x1b[%d;0H%32s", row, ""); }

void centred(int row, const char *text) {
	const int len = (int)strlen(text);
	clearRow(row);
	at(len >= 32 ? 0 : (32 - len) / 2, row, text);
}

void drawButton(const Button &b) {
	char line[40];
	auto fill = [&](char edge, char mid) {
		for (int i = 0; i < b.w; i++)
			line[i] = (i == 0 || i == b.w - 1) ? edge : mid;
		line[b.w] = 0;
	};
	fill('+', '-');
	at(b.x, b.y, line);
	fill('|', ' ');
	for (int r = 1; r < b.h - 1; r++)
		at(b.x, b.y + r, line);
	fill('+', '-');
	at(b.x, b.y + b.h - 1, line);

	auto label = [&](int row, const char *text) {
		const int len = (int)strlen(text);
		if (len <= b.w - 2)
			at(b.x + (b.w - len) / 2, row, text);
	};
	label(b.y + 1, b.line1);
	label(b.y + 2, b.line2);
}

bool hit(const Button &b, const touchPosition &t) {
	const int tx = t.px / 8, ty = t.py / 8;
	return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
}

//---------------------------------------------------------------- video

void clearPreviewPages() {
	for (int i = 0; i < 2; i++)
		dmaFillHalfWords(0x8000, previewPage[i], 256 * 256 * 2);
}

void initVideo() {
	videoSetMode(MODE_5_2D);
	videoSetModeSub(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
	vramSetBankC(VRAM_C_SUB_BG_0x06200000);

	bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	bgSetMapBase(3, 0);
	clearPreviewPages();

	consoleInit(&subConsole, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
	BG_PALETTE_SUB[0] = RGB15(26, 29, 31);   // light blue-grey background
	BG_PALETTE_SUB[255] = RGB15(2, 8, 16);   // dark text
	iprintf("\x1b[2J");

	setBrightness(3, 0);
}

void showPage(int page) { bgSetMapBase(3, page ? 8 : 0); } // 8 * 16 KB = second 128 KB bitmap

void flashTop() {
	setBrightness(1, 16);
	for (int i = 0; i < 4; i++)
		swiWaitForVBlank();
	for (int level = 16; level >= 0; level -= 4) {
		setBrightness(1, level);
		swiWaitForVBlank();
	}
}

//---------------------------------------------------------------- leaving

void waitFrames(int n) {
	for (int i = 0; i < n; i++)
		swiWaitForVBlank();
}

[[noreturn]] void stopForever() {
	while (true)
		swiWaitForVBlank();
}

void returnToMenu() {
	cameraShutdown();

	setBrightness(3, 16); // fade to white
	waitFrames(8);

	const char *menu = nullptr;
	switch (ms().theme) {
		case TWLSettings::EThemeR4:
		case TWLSettings::EThemeGBC:
			menu = sys().isRunFromSD() ? "sd:/_nds/TWiLightMenu/r4menu.srldr" : "fat:/_nds/TWiLightMenu/r4menu.srldr";
			break;
		case TWLSettings::EThemeWood:
			menu = sys().isRunFromSD() ? "sd:/_nds/TWiLightMenu/akmenu.srldr" : "fat:/_nds/TWiLightMenu/akmenu.srldr";
			break;
		default:
			menu = sys().isRunFromSD() ? "sd:/_nds/TWiLightMenu/dsimenu.srldr" : "fat:/_nds/TWiLightMenu/dsimenu.srldr";
			break;
	}

	std::vector<const char *> argv;
	argv.push_back(menu);
	runNdsFile(menu, argv.size(), &argv[0], sys().isRunFromSD(), true, false, false, true, true, false, -1);

	// The menu could not be started: fall back to BOOT.NDS, like the other TWiLight apps do.
	runNdsFile(sys().isRunFromSD() ? "sd:/boot.nds" : "fat:/boot.nds", 0, NULL, sys().isRunFromSD(), true, true, false, true, true, false, -1);

	setBrightness(3, 0);
	iprintf("\x1b[2J");
	centred(8, "Could not return to menu.");
	centred(10, "Hold the power button.");
	stopForever();
}

// Power button / START+SELECT+L+R from the ARM7 (see arm7/source/main.c)
bool powerExitRequested() { return fifoCheckValue32(FIFO_USER_01); }

void lidSleep(Camera resume) {
	cameraTransferStop();
	cameraDeactivateActive();
	if (!ms().macroMode)
		powerOff(PM_BACKLIGHT_TOP);
	powerOff(PM_BACKLIGHT_BOTTOM);
	while (keysHeld() & KEY_LID) {
		scanKeys();
		swiWaitForVBlank();
	}
	if (!ms().macroMode)
		powerOn(PM_BACKLIGHT_TOP);
	powerOn(PM_BACKLIGHT_BOTTOM);
	if (resume != CAM_NONE)
		cameraActivate(resume);
}

//---------------------------------------------------------------- error screen

void fatalError(const char *line1, const char *line2 = "") {
	cameraShutdown();
	setBrightness(3, 0);
	iprintf("\x1b[2J");
	centred(6, "nerdMod Camera");
	centred(9, line1);
	if (line2[0])
		centred(11, line2);
	Button back = {9, 15, 14, 4, "OK", "(B / A)"};
	drawButton(back);
	while (true) {
		scanKeys();
		touchPosition t;
		touchRead(&t);
		const u32 down = keysDown();
		if ((down & (KEY_A | KEY_B)) || ((down & KEY_TOUCH) && hit(back, t)) || powerExitRequested())
			break;
		swiWaitForVBlank();
	}
	returnToMenu();
}

//---------------------------------------------------------------- album

enum class AlbumExit { Back, PowerExit };

bool confirmDelete(const std::string &name) {
	iprintf("\x1b[2J");
	centred(5, "Delete this photo?");
	centred(7, name.c_str());
	Button yes = {1, 12, 14, 4, "DELETE", "(A)"};
	Button no = {17, 12, 14, 4, "CANCEL", "(B)"};
	drawButton(yes);
	drawButton(no);
	while (true) {
		scanKeys();
		touchPosition t;
		touchRead(&t);
		const u32 down = keysDown();
		if ((down & KEY_A) || ((down & KEY_TOUCH) && hit(yes, t)))
			return true;
		if ((down & KEY_B) || ((down & KEY_TOUCH) && hit(no, t)))
			return false;
		if (powerExitRequested()) {
			exitRequested = true;
			return false;
		}
		swiWaitForVBlank();
	}
}

AlbumExit albumMode() {
	// The camera is not needed here: switch it off (LED, power) while browsing.
	cameraDeactivateActive();

	std::vector<std::string> names;
	photosList(names, MAX_ALBUM_ENTRIES);

	const Button prev = {0, 14, 8, 4, "PREV", "(L)"};
	const Button next = {8, 14, 8, 4, "NEXT", "(R)"};
	const Button del = {16, 14, 8, 4, "DEL", "(X)"};
	const Button back = {24, 14, 8, 4, "BACK", "(B)"};

	int index = (int)names.size() - 1; // newest first
	bool redraw = true;

	while (true) {
		if (redraw) {
			redraw = false;
			iprintf("\x1b[2J");
			centred(1, "Album");
			drawButton(prev);
			drawButton(next);
			drawButton(del);
			drawButton(back);

			if (names.empty()) {
				dmaFillHalfWords(0x8000, previewPage[0], 256 * 192 * 2);
				showPage(0);
				centred(7, "No photos yet.");
				centred(9, "Take one with the shutter!");
			} else {
				char count[32];
				snprintf(count, sizeof(count), "%d / %d", index + 1, (int)names.size());
				centred(3, count);
				centred(5, names[index].c_str());
				if (photosDrawScaled(names[index], previewPage[0])) {
					showPage(0);
				} else {
					dmaFillHalfWords(0x8000, previewPage[0], 256 * 192 * 2);
					showPage(0);
					centred(8, "Cannot show this photo.");
					centred(10, "(corrupt or unsupported)");
				}
			}
		}

		scanKeys();
		touchPosition t;
		touchRead(&t);
		const u32 down = keysDown();

		if (powerExitRequested())
			return AlbumExit::PowerExit;
		if (down & KEY_LID) {
			lidSleep(CAM_NONE);
			continue;
		}

		if ((down & KEY_B) || ((down & KEY_TOUCH) && hit(back, t)))
			return AlbumExit::Back;

		if (!names.empty()) {
			if ((down & (KEY_LEFT | KEY_L)) || ((down & KEY_TOUCH) && hit(prev, t))) {
				index = (index + (int)names.size() - 1) % (int)names.size();
				redraw = true;
			} else if ((down & (KEY_RIGHT | KEY_R)) || ((down & KEY_TOUCH) && hit(next, t))) {
				index = (index + 1) % (int)names.size();
				redraw = true;
			} else if ((down & KEY_X) || ((down & KEY_TOUCH) && hit(del, t))) {
				if (confirmDelete(names[index])) {
					if (!photosDelete(names[index])) {
						iprintf("\x1b[2J");
						centred(8, "Could not delete photo.");
						waitFrames(90);
					} else {
						names.erase(names.begin() + index);
						if (index >= (int)names.size())
							index = (int)names.size() - 1;
					}
				}
				if (exitRequested)
					return AlbumExit::PowerExit;
				redraw = true;
			}
		}

		swiWaitForVBlank();
	}
}

//---------------------------------------------------------------- camera screen

const Button btnShutter = {1, 7, 14, 4, "SHUTTER", "(A)"};
const Button btnSwitch = {17, 7, 14, 4, "SWITCH", "(X / L / R)"};
const Button btnAlbum = {1, 13, 14, 4, "ALBUM", "(Y)"};
const Button btnBack = {17, 13, 14, 4, "BACK", "(B)"};

void drawCameraScreen(Camera cam, const char *status) {
	iprintf("\x1b[2J");
	centred(1, "nerdMod Camera");
	centred(3, cam == CAM_INNER ? "Camera: Inner (front)" : "Camera: Outer (rear)");
	if (status && status[0])
		centred(5, status);
	drawButton(btnShutter);
	drawButton(btnSwitch);
	drawButton(btnAlbum);
	drawButton(btnBack);
}

// Waits (bounded) for the running preview transfer to finish.
bool waitTransferIdle(int maxFrames) {
	for (int i = 0; i < maxFrames; i++) {
		if (!cameraTransferActive())
			return true;
		swiWaitForVBlank();
	}
	return false;
}

// Captures a full-size frame, shows it on the top screen and saves it.
// Returns the status line to show.
std::string takePhoto(Camera cam, int &frontPage) {
	if (!waitTransferIdle(60))
		return "Camera busy, try again.";
	cameraTransferStop();

	u16 *yuv = (u16 *)malloc(CAM_CAPTURE_BYTES);
	if (!yuv)
		return "Out of memory.";

	std::string result;
	if (!cameraTransferStart(yuv, CAPTURE_MODE_CAPTURE)) {
		result = "Capture failed.";
	} else if (!waitTransferIdle(60)) {
		cameraTransferStop();
		result = "Capture timed out.";
	} else {
		cameraTransferStop();

		// Immediate feedback: flash and show what was captured
		photosDrawYuvScaled(yuv, previewPage[frontPage]);
		flashTop();

		std::string name;
		switch (photosSaveYuv(yuv, name)) {
			case PHOTO_OK:
				result = "Saved " + name;
				break;
			case PHOTO_NO_STORAGE:
				result = "SD card not available.";
				break;
			default:
				result = "Save failed (SD full?).";
				break;
		}
		waitFrames(60); // keep the captured photo on screen for a moment
	}

	free(yuv);
	(void)cam;
	return result;
}

// Runs the live camera until the user leaves. Returns true to keep the app running
// (album was opened and closed) or false to exit.
void cameraMode() {
	Camera cam = CAM_OUTER;
	if (!cameraActivate(cam))
		fatalError("The camera did not start.", "Close the lid, then retry.");

	std::string status;
	drawCameraScreen(cam, status.c_str());

	int front = 0; // page currently shown
	bool inFlight = false;

	while (!exitRequested) {
		// ---- preview: double-buffered so the picture never tears
		if (!cameraTransferActive()) {
			if (inFlight) {
				front ^= 1;
				showPage(front);
				inFlight = false;
			}
			if (!cameraTransferStart(previewPage[front ^ 1], CAPTURE_MODE_PREVIEW))
				fatalError("The camera stopped responding.");
			inFlight = true;
		}

		scanKeys();
		touchPosition t;
		touchRead(&t);
		const u32 down = keysDown();

		if (powerExitRequested()) {
			exitRequested = true;
			break;
		}

		if (down & KEY_LID) {
			waitTransferIdle(10);
			lidSleep(cam);
			inFlight = false;
			continue;
		}

		const bool wantShutter = (down & KEY_A) || ((down & KEY_TOUCH) && hit(btnShutter, t));
		const bool wantSwitch = (down & (KEY_X | KEY_L | KEY_R)) || ((down & KEY_TOUCH) && hit(btnSwitch, t));
		const bool wantAlbum = (down & KEY_Y) || ((down & KEY_TOUCH) && hit(btnAlbum, t));
		const bool wantBack = (down & KEY_B) || ((down & KEY_TOUCH) && hit(btnBack, t));

		if (wantBack)
			break;

		if (wantShutter) {
			status = takePhoto(cam, front);
			drawCameraScreen(cam, status.c_str());
			inFlight = false;
		} else if (wantSwitch) {
			waitTransferIdle(30);
			cameraTransferStop();
			const Camera other = (cam == CAM_INNER) ? CAM_OUTER : CAM_INNER;
			if (cameraActivate(other)) {
				cam = other;
				status.clear();
			} else {
				status = "Could not switch camera.";
				cameraActivate(cam);
			}
			inFlight = false;
			drawCameraScreen(cam, status.c_str());
		} else if (wantAlbum) {
			waitTransferIdle(30);
			cameraTransferStop();
			inFlight = false;
			const AlbumExit result = albumMode();
			if (result == AlbumExit::PowerExit) {
				exitRequested = true;
				break;
			}
			if (!cameraActivate(cam))
				fatalError("The camera did not start.");
			clearPreviewPages();
			status.clear();
			drawCameraScreen(cam, status.c_str());
		}

		swiWaitForVBlank();
	}
}

} // namespace

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	fifoSendValue32(FIFO_PM, PM_REQ_SLEEP_DISABLE); // we handle the lid ourselves
	defaultExceptionHandler();

	sys().initFilesystem(argc == 0 ? "sd:/_nds/TWiLightMenu/main.srldr" : argv[0]);
	sys().initArm7RegStatuses();

	useTwlCfg = (dsiFeatures() && (*(u8 *)0x02000400 != 0) && (*(u8 *)0x02000401 == 0) && (*(u8 *)0x02000402 == 0) && (*(u8 *)0x02000404 == 0) && (*(u8 *)0x02000448 != 0));

	initVideo();

	if (sys().fatInitOk())
		ms().loadSettings();

	if (!dsiFeatures() || REG_SCFG_EXT == 0) {
		fatalError("This needs a Nintendo DSi", "running in DSi mode.");
	}
	if (!sys().fatInitOk()) {
		fatalError("No SD card found.", "Photos cannot be saved.");
	}

	keysSetRepeat(25, 5);

	centred(10, "Starting camera...");
	if (!cameraInit()) {
		fatalError("Camera hardware not found.", "(init failed)");
	}

	cameraMode();

	returnToMenu();
	return 0;
}
