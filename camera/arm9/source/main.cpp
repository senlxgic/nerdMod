/*
	nerdMod Camera

	A small Nintendo DSi camera application for nerdMod:
	live preview from the inner or outer camera (I2C 0x7A / 0x78), photos (640x480 BMP) and videos (256x192, about
	10 fps, NERDVID) on the SD card, and an album that plays them back. Launched like any other app from the menu;
	it returns to the menu it was started from.

	Controls (the touch buttons mirror them):
	  Camera:  A shutter / record | X or L/R switch camera | SELECT photo <-> video | Y album | B back
	           (B while recording stops the recording)
	  Album:   Left/Right or L/R browse | A play (videos) | X delete | B back
	  Player:  A play/pause | Left/Right seek 5 s | B back
*/

#include <nds.h>

#include <fat.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "app.h"
#include "camera.h"
#include "camsettings.h"
#include "fpsutil.h"
#include "audioRecorder.h"
#include "audioPlayer.h"
#include "reclog.h"
#include "sdbench.h"
#include "settings.h"
#include "gallery.h"
#include "filters.h"
#include "photos.h"
#include "ui.h"
#include "video.h"
#include "videoContainer.h"

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

bool exitRequested = false;

enum { B_SHUTTER = 1, B_ALBUM, B_FLIP, B_BACK, B_MODE, B_OK, B_SETTINGS, B_FPS };

//---------------------------------------------------------------- leaving

[[noreturn]] void stopForever() {
	while (true)
		swiWaitForVBlank();
}

[[noreturn]] void returnToMenu() {
	cameraShutdown();

	uiTopFade(true, 6); // fade to white
	setBrightness(3, 16);
	appWaitFrames(4);

	// Same choice as the other TWiLight apps (see manual/arm9/source/main.cpp: loadROMselect)
	const bool sd = sys().isRunFromSD();
	const char *menu = nullptr;
	switch (ms().theme) {
		case TWLSettings::EThemeDSi:
		case TWLSettings::EThemeHBL:
		case TWLSettings::EThemeSaturn:
			if (!ms().showSelectMenu) {
				menu = sd ? "sd:/_nds/TWiLightMenu/mainmenu.srldr" : "fat:/_nds/TWiLightMenu/mainmenu.srldr";
				break;
			}
			// fall through
		case TWLSettings::ETheme3DS:
		default:
			menu = sd ? "sd:/_nds/TWiLightMenu/dsimenu.srldr" : "fat:/_nds/TWiLightMenu/dsimenu.srldr";
			break;
		case TWLSettings::EThemeR4:
		case TWLSettings::EThemeGBC:
			menu = sd ? "sd:/_nds/TWiLightMenu/r4menu.srldr" : "fat:/_nds/TWiLightMenu/r4menu.srldr";
			break;
		case TWLSettings::EThemeWood:
			menu = sd ? "sd:/_nds/TWiLightMenu/mainmenu.srldr" : "fat:/_nds/TWiLightMenu/mainmenu.srldr";
			break;
	}

	std::vector<const char *> argv;
	argv.push_back(menu);
	runNdsFile(menu, argv.size(), &argv[0], sys().isRunFromSD(), true, false, false, true, true, false, -1);

	// The menu could not be started: fall back to BOOT.NDS, like the other TWiLight apps do.
	runNdsFile(sys().isRunFromSD() ? "sd:/boot.nds" : "fat:/boot.nds", 0, NULL, sys().isRunFromSD(), true, true, false, true, true, false, -1);

	setBrightness(3, 0);
	uiTextClear();
	uiTextCentred(8, "Could not return to menu.");
	uiTextCentred(10, "Hold the power button.");
	stopForever();
}

int fpsPillImage(int fps) {
	switch (fps) {
		case 15: return UI_BTN_FPS15;
		case 20: return UI_BTN_FPS20;
		case 30: return UI_BTN_FPS30;
		default: return UI_BTN_FPS10;
	}
}

void showCameraButtons(bool videoMode, bool recording, bool glow = false) {
	const int shutterN = recording ? (glow ? UI_SHUTTER_REC_GLOW : UI_SHUTTER_REC) : (videoMode ? UI_SHUTTER_VIDEO : UI_SHUTTER_PHOTO);
	const int shutterP = recording ? UI_SHUTTER_REC_P : (videoMode ? UI_SHUTTER_VIDEO_P : UI_SHUTTER_PHOTO_P);
	UiButton list[7] = {
		{B_SHUTTER, UI_RECT_SHUTTER, shutterN, shutterP, true},
		{B_ALBUM, UI_RECT_ALBUM, UI_BTN_ALBUM, UI_BTN_ALBUM_P, !recording},
		{B_FLIP, UI_RECT_FLIP, UI_BTN_FLIP, UI_BTN_FLIP_P, !recording},
		{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true},
		{B_MODE, UI_RECT_CAPSULE, videoMode ? UI_CAPSULE_VIDEO : UI_CAPSULE_PHOTO, -1, !recording},
		{B_SETTINGS, UI_RECT_GEAR, UI_BTN_GEAR, UI_BTN_GEAR_P, !recording},
	};
	int n = 6;
	// the tappable frame-rate pill only exists in video mode, and not while recording
	if (videoMode && !recording)
		list[n++] = {B_FPS, UI_RECT_FPS, fpsPillImage(rec::fps()), UI_BTN_FPS_P, true};
	uiBottomDrawBackground();
	uiShowButtons(list, n);
}

//---------------------------------------------------------------- error screen

void fatalError(const char *line1, const char *line2 = "") {
	if (rec::active())
		rec::stop(rec::STOP_USER); // keep what was recorded so far
	cameraShutdown();
	setBrightness(3, 0);
	uiTopOverlayVisible(false);
	uiClearButtons();
	uiTextClear();
	uiBottomDrawBackground();
	uiDrawDialogPanel();
	const UiButton ok[] = {{B_OK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true}};
	uiShowButtons(ok, 1);
	uiTextCentred(7, "nerdMod Camera");
	uiTextCentred(10, line1);
	if (line2[0])
		uiTextCentred(12, line2);
	while (true) {
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if ((down & (KEY_A | KEY_B)) || touched == B_OK || appPowerExitRequested())
			break;
		swiWaitForVBlank();
	}
	returnToMenu();
}

//---------------------------------------------------------------- camera screen

// Waits (bounded) for the running preview transfer to finish.
bool waitTransferIdle(int maxFrames) {
	for (int i = 0; i < maxFrames; i++) {
		if (!cameraTransferActive())
			return true;
		swiWaitForVBlank();
	}
	return false;
}

// Captures a full-size frame, shows it on the top screen and saves it. Returns the status line to show.
const char *takePhoto(int &frontPage) {
	if (!waitTransferIdle(60))
		return "Camera busy";
	cameraTransferStop();

	u16 *yuv = (u16 *)memalign(32, CAM_CAPTURE_BYTES); // 32-byte aligned, size is a multiple of 32
	if (!yuv)
		return "Out of memory";

	const char *result;
	if (!cameraTransferStart(yuv, CAPTURE_MODE_CAPTURE)) {
		result = "Capture failed";
	} else if (!waitTransferIdle(60)) {
		cameraTransferStop();
		result = "Capture timed out";
	} else {
		cameraTransferStop();

		// Immediate feedback: show what was captured, with a white flash
		const int shownPage = frontPage ^ 1;
		photosDrawYuvScaled(yuv, uiTopPage(shownPage));
		uiTopShowPage(shownPage);
		frontPage = shownPage;
		uiTopFlash();

		std::string name;
		switch (photosSaveYuv(yuv, name)) {
			case PHOTO_OK:
				result = "Photo saved";
				break;
			case PHOTO_NO_STORAGE:
				result = "No SD card";
				break;
			case PHOTO_NO_SPACE:
				result = "SD card full";
				break;
			default:
				result = "Save failed";
				break;
		}
		appWaitFrames(45); // keep the captured photo on screen for a moment
	}

	free(yuv);
	return result;
}

//---------------------------------------------------------------- recording info

const char *fpsWord(int fps) {
	switch (fps) {
		case 15: return "Smooth";
		case 20: return "High";
		case 30: return "Max";
		default: return "Safe";
	}
}

rec::Result lastRecording;
bool haveLastRecording = false;

const char *messageForStop(const rec::Result &r) {
	if (!r.saved)
		return r.frames == 0 ? "Nothing recorded" : "Video not saved";
	switch (r.reason) {
		case rec::STOP_SD_FULL: return "SD full - saved";
		case rec::STOP_TOO_SLOW: return "SD slow - saved";
		case rec::STOP_WRITE_ERROR: return "Write error-saved";
		case rec::STOP_LIMIT: return "Limit - saved";
		default: return "Video saved";
	}
}

void cameraMode() {
	// Start with the outer camera if it works, otherwise the inner one
	Camera cam = cameraAvailable(CAM_OUTER) ? CAM_OUTER : CAM_INNER;
	if (!cameraActivate(cam))
		fatalError("The camera did not start.", "Leave and open Camera again.");

	bool videoMode = false;
	fx::set(fx::NORMAL);
	rec::setFps(camsettings::videoFps());
	uiTopSetCamera(cam == CAM_INNER);
	uiTopSetMode(false);
	showCameraButtons(false, false);
	uiStatus("Ready");

	int front = 0;			// top page currently shown
	bool inFlight = false;
	int framesInFlight = 0; // watchdog for a preview transfer that never completes
	int stalls = 0;
	int glowCounter = 0;
	bool glow = false;
	int messageFrames = 0;
	const char *lastStatus = "";
	char lastRecLine[24] = "";

	auto setMessage = [&](const char *m) {
		uiStatus(m);
		lastStatus = m;
		messageFrames = 150;
	};
	// The idle status line: the active effect, else the mode
	auto idleLabel = [&]() -> const char * {
		const char *fxLabel = fx::statusLabel(fx::current());
		static char videoLabel[24];
		if (!fxLabel && videoMode) {
			snprintf(videoLabel, sizeof(videoLabel), "Video %d FPS", rec::fps());
			return videoLabel;
		}
		return fxLabel ? fxLabel : "Ready";
	};
	auto setIdleStatus = [&](const char *m) {
		if (m != lastStatus) {
			uiStatus(m);
			lastStatus = m;
		}
	};

	auto restoreCameraScreen = [&]() {
		uiTextClear();
		showCameraButtons(videoMode, false);
		lastStatus = "";
		setIdleStatus(idleLabel());
	};

	auto stopRecording = [&](rec::StopReason why) {
		waitTransferIdle(20);
		cameraTransferStop();
		inFlight = false;
		uiStatus("Saving...");
		const rec::Result r = rec::stop(why);
		uiTopSetRecording(false, 0);
		showCameraButtons(videoMode, false);
		setMessage(messageForStop(r));
		lastRecLine[0] = 0;
		if (r.frames > 0) {
			lastRecording = r;
			haveLastRecording = true;
			if (camset::recordingSummary() == camset::Exit::Power)
				exitRequested = true;
			restoreCameraScreen();
			setMessage(messageForStop(r));
		}
	};

	auto startRecording = [&]() {
		waitTransferIdle(30);
		cameraTransferStop();
		inFlight = false;
		std::string error;
		if (!rec::start(cam == CAM_INNER, error)) {
			setMessage(error.c_str());
			return;
		}
		glowCounter = 0;
		glow = false;
		showCameraButtons(videoMode, true);
		uiTopSetRecording(true, 0);
		lastStatus = "";
		lastRecLine[0] = 0;
		setIdleStatus("Recording");
	};

	setMessage("D-pad: effects");

	while (!exitRequested) {
		// ---- preview: double-buffered so the picture never tears. While recording the frames land in RAM
		//      buffers first (see video.h) and are copied to the screen page.
		if (!cameraTransferActive()) {
			if (inFlight) {
				inFlight = false;
				const int back = front ^ 1;
				if (rec::active()) {
					rec::frameCaptured();
					const u16 *f = rec::lastFrame();
					if (f) {
						if (fx::current() != fx::NORMAL) {
							// effect on the RAM frame: it is what gets saved in the video and what the screen shows
							fx::applyFrame((u16 *)f);
							DC_FlushRange(f, nvid::FRAME_BYTES); // DMA reads RAM, not the cache
						}
						dmaCopyHalfWords(3, f, uiTopPage(back), nvid::FRAME_BYTES);
					}
				} else {
					fx::applyFrame(uiTopPage(back)); // live effect on the finished preview page (no-op for NORMAL)
				}
				front = back;
				uiTopShowPage(front);
			}
			u16 *target = rec::active() ? rec::captureTarget() : uiTopPage(front ^ 1);
			if (!target || !cameraTransferStart(target, CAPTURE_MODE_PREVIEW)) {
				if (rec::active())
					stopRecording(rec::STOP_USER);
				fatalError("The camera stopped responding.");
			}
			inFlight = true;
			framesInFlight = 0;
		} else if (++framesInFlight > 120) {
			// A preview frame normally lands within a few vblanks. Abort, restart, and
			// give up if it keeps happening.
			cameraTransferStop();
			inFlight = false;
			if (++stalls >= 5) {
				if (rec::active())
					stopRecording(rec::STOP_USER);
				fatalError("The camera stopped responding.", "(no image data)");
			}
		}

		// ---- recording housekeeping: at most one SD write per iteration
		if (rec::active()) {
			rec::pump();
			uiTopSetRecording(true, rec::elapsedMs() / 1000);
			if (++glowCounter >= 30) {
				glowCounter = 0;
				glow = !glow;
				uiSetButtonImages(B_SHUTTER, glow ? UI_SHUTTER_REC_GLOW : UI_SHUTTER_REC, UI_SHUTTER_REC_P);
			}
			const rec::StopReason why = rec::autoStopReason();
			if (why != rec::STOP_NONE) {
				stopRecording(why);
			} else if (messageFrames == 0) {
				if (rec::queuedFrames() >= 5)
					setIdleStatus("SD card is slow...");
				else if (rec::elapsedMs() > 2500 && !rec::hasMicData())
					setIdleStatus("Rec, no sound");
				else
					setIdleStatus("Recording");
			}
		}

		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		int action = uiHandleInput(down, up);
		uiTick();

		if (action < 0) {
			const bool rc = rec::active();
			if (down & KEY_A)
				action = B_SHUTTER;
			else if (down & KEY_B)
				action = B_BACK;
			else if (!rc && (down & (KEY_X | KEY_L | KEY_R)))
				action = B_FLIP;
			else if (!rc && (down & KEY_Y))
				action = B_ALBUM;
			else if (!rc && (down & KEY_SELECT))
				action = B_MODE;
			if (action >= 0)
				uiPressFeedback(action);
		}

		// ---- START: video frame rate shortcut (video mode) / Camera Settings (photo mode)
		if (!rec::active() && action < 0 && (down & KEY_START)) {
			if (videoMode) {
				const int next = fpsutil::next(rec::fps());
				rec::setFps(next);
				camsettings::setVideoFps(next);
				static char fpsMsg[24];
				snprintf(fpsMsg, sizeof(fpsMsg), "%d %s %dMB/min", next, fpsWord(next), fpsutil::mbPerMinute(next));
				setMessage(fpsMsg);
				showCameraButtons(videoMode, false);
			} else {
				action = B_SETTINGS;
			}
		}

		// ---- effects: D-pad LEFT/RIGHT, or a tap on the status bar (next effect)
		if (!rec::active() && action < 0) {
			int dir = 0;
			if (down & KEY_RIGHT)
				dir = 1;
			else if (down & KEY_LEFT)
				dir = -1;
			else if (down & KEY_TOUCH) {
				touchPosition tp;
				touchRead(&tp);
				if (tp.px >= 92 && tp.px < 92 + 156 && tp.py >= 158 && tp.py < 158 + 26)
					dir = 1;
			}
			if (dir != 0) {
				const fx::Effect next = fx::step(fx::current(), dir, videoMode);
				if (!fx::set(next))
					setMessage("Effect: no memory");
				else
					setMessage(fx::statusLabel(fx::current()) ? fx::statusLabel(fx::current()) : "Effect: NORMAL");
			}
		}

		if (appPowerExitRequested()) {
			if (rec::active())
				stopRecording(rec::STOP_USER);
			exitRequested = true;
			break;
		}

		if (down & KEY_LID) {
			if (rec::active())
				stopRecording(rec::STOP_USER);
			waitTransferIdle(10);
			appLidSleep(cam);
			inFlight = false;
			continue;
		}

		if (messageFrames > 0 && --messageFrames == 0 && !rec::active()) {
			lastStatus = "";
			setIdleStatus(idleLabel());
		}

		if (action == B_BACK) {
			if (rec::active()) {
				stopRecording(rec::STOP_USER);
			} else {
				break;
			}
		} else if (action == B_SHUTTER) {
			if (rec::active()) {
				stopRecording(rec::STOP_USER);
			} else if (videoMode) {
				startRecording();
			} else {
				setMessage(takePhoto(front));
				inFlight = false;
			}
		} else if (action == B_MODE && !rec::active()) {
			videoMode = !videoMode;
			bool effectDropped = false;
			if (videoMode && !fx::videoSupported(fx::current())) {
				fx::set(fx::NORMAL); // this effect is photo-only
				effectDropped = true;
			}
			uiTopSetMode(videoMode);
			showCameraButtons(videoMode, false);
			setMessage(effectDropped ? "Video: effect off" : (videoMode ? "Video mode" : "Photo mode"));
		} else if (action == B_FLIP && !rec::active()) {
			waitTransferIdle(30);
			cameraTransferStop();
			inFlight = false;
			uiTopFade(true, 4);
			const Camera other = (cam == CAM_INNER) ? CAM_OUTER : CAM_INNER;
			if (cameraActivate(other)) {
				cam = other;
				setMessage(cam == CAM_INNER ? "Inner camera" : "Outer camera");
			} else {
				setMessage("Switch failed");
				cameraActivate(cam);
			}
			uiTopSetCamera(cam == CAM_INNER);
			uiTopFade(false, 4);
		} else if ((action == B_SETTINGS || action == B_FPS) && !rec::active()) {
			waitTransferIdle(30);
			cameraTransferStop();
			inFlight = false;
			camset::Exit e = camset::Exit::Back;
			if (action == B_FPS)
				e = camset::chooseFps(nullptr);
			else
				e = camset::run();
			if (e == camset::Exit::Power) {
				exitRequested = true;
				break;
			}
			uiTextClear();
			showCameraButtons(videoMode, false);
			lastStatus = "";
			setIdleStatus(idleLabel());
		} else if (action == B_ALBUM && !rec::active()) {
			waitTransferIdle(30);
			cameraTransferStop();
			inFlight = false;
			uiTopFade(true, 4);
			const AlbumExit result = galleryRun();
			if (result == AlbumExit::PowerExit) {
				exitRequested = true;
				break;
			}
			if (!cameraActivate(cam))
				fatalError("The camera did not start.");
			uiTopClear();
			front = uiTopShownPage();
			uiTopSetCamera(cam == CAM_INNER);
			uiTextClear();
			showCameraButtons(videoMode, false);
			lastStatus = "";
			setIdleStatus(idleLabel());
			uiTopFade(false, 4);
		}

		swiWaitForVBlank();
	}

	if (rec::active())
		rec::stop(rec::STOP_USER);
}

} // namespace

//---------------------------------------------------------------- services for gallery / player
bool appPowerExitRequested() { return fifoCheckValue32(FIFO_USER_01); } // from the ARM7 (see arm7/source/main.c)

void appWaitFrames(int frames) {
	for (int i = 0; i < frames; i++)
		swiWaitForVBlank();
}

void appLidSleep(Camera resume) {
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

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	fifoSendValue32(FIFO_PM, PM_REQ_SLEEP_DISABLE); // we handle the lid ourselves
	defaultExceptionHandler();

	sys().initFilesystem(argc == 0 ? "sd:/_nds/TWiLightMenu/main.srldr" : argv[0]);
	sys().initArm7RegStatuses();

	useTwlCfg = (dsiFeatures() && (*(u8 *)0x02000400 != 0) && (*(u8 *)0x02000401 == 0) && (*(u8 *)0x02000402 == 0) && (*(u8 *)0x02000404 == 0) && (*(u8 *)0x02000448 != 0));

	uiInit();

	if (sys().fatInitOk())
		ms().loadSettings();

	if (!dsiFeatures() || !cameraHardwareAccessible()) {
		// Not a DSi, or started in DS mode / with SCFG locked: the camera registers are
		// not reachable, so do not touch them.
		fatalError("This needs a Nintendo DSi", "running in DSi mode.");
	}
	if (!sys().fatInitOk()) {
		fatalError("No SD card found.", "Photos cannot be saved.");
	}

	keysSetRepeat(25, 5);

	uiStatus("Starting...");
	if (!cameraInit()) {
		if (cameraLastError() == CAM_ERR_NO_ACCESS)
			fatalError("This needs a Nintendo DSi", "running in DSi mode.");
		else
			fatalError("Camera hardware not found.", "(sensor did not answer)");
	}

	cameraMode();

	returnToMenu();
	return 0;
}
