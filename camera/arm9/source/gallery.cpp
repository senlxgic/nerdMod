#include "gallery.h"

#include <algorithm>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "app.h"
#include "photos.h"
#include "player.h"
#include "ui.h"
#include "video.h"
#include "videoContainer.h"

namespace {

constexpr size_t MAX_ITEMS = 512;

enum { B_PREV = 1, B_PLAY, B_DELETE, B_NEXT, B_BACK, B_YES, B_NO };

bool endsWith(const char *name, const char *ext) {
	const size_t n = strlen(name), e = strlen(ext);
	return n > e && strcasecmp(name + n - e, ext) == 0;
}

bool isVideoName(const char *name) { return strlen(name) > 8 && strncasecmp(name, "NV_", 3) == 0 && endsWith(name, ".nvid"); }
bool isPhotoName(const char *name) { return strlen(name) > 7 && strncasecmp(name, "NM_", 3) == 0 && endsWith(name, ".bmp"); }

std::string itemPath(const MediaItem &m) { return (m.video ? rec::videoFolder() : photosDirectory()) + "/" + m.name; }

// "NM_20260105_142233.bmp" -> "2026-01-05  14:22:33"
std::string dateOf(const std::string &name) {
	int y, mo, d, h, mi, s;
	if (name.size() > 18 && sscanf(name.c_str() + 3, "%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d:%02d", y, mo, d, h, mi, s);
		return buf;
	}
	return name;
}

struct Shown {
	bool ok = false;
	std::string line1, line2;
	bool playable = false;
};

void clearTop(int page) { dmaFillHalfWords(0x8000, uiTopPage(page), nvid::FRAME_BYTES); }

// Puts the thumbnail (video: first frame, photo: scaled bitmap) on the top screen and describes the item.
Shown showItem(const MediaItem &m, u16 *scratch, int &page) {
	Shown s;
	const int next = page ^ 1;
	u16 *dst = uiTopPage(next);
	if (m.video) {
		nvid::Reader r;
		if (r.open(itemPath(m)) && r.readFrame(0, scratch)) {
			DC_FlushRange(scratch, nvid::FRAME_BYTES); // written by the CPU, copied by DMA
			dmaCopyHalfWords(3, scratch, dst, nvid::FRAME_BYTES);
			s.ok = s.playable = true;
			const u32 sec = (r.durationMs() + 500) / 1000;
			char buf[48];
			snprintf(buf, sizeof(buf), "Video  %u:%02u  %s", (unsigned)(sec / 60), (unsigned)(sec % 60), r.hasAudio() ? "(sound saved)" : "");
			s.line1 = buf;
			if (r.info().flags & nvid::FLAG_FRAMES_DROPPED) {
				snprintf(buf, sizeof(buf), "Video  %u:%02u  (frames skipped)", (unsigned)(sec / 60), (unsigned)(sec % 60));
				s.line1 = buf;
			}
			// Phase 2C.1: what the file really contains (requested / actual rate, sound bytes), so recording and playback problems can be told apart
			const nvid::Header &hd = r.info();
			const u32 dur = r.durationMs();
			const u32 actX10 = dur ? (u32)(((u64)r.frameCount() * 10000ull + dur / 2) / dur) : 0;
			if (hd.audioBytes > 0)
				snprintf(buf, sizeof(buf), "%ufps req %u.%ufps  snd %uKB", (unsigned)hd.fpsNum, (unsigned)(actX10 / 10), (unsigned)(actX10 % 10), (unsigned)(hd.audioBytes / 1024));
			else
				snprintf(buf, sizeof(buf), "%ufps req %u.%ufps  NO SOUND", (unsigned)hd.fpsNum, (unsigned)(actX10 / 10), (unsigned)(actX10 % 10));
			s.line2 = buf;
		}
	} else if (photosDrawScaled(m.name, dst)) {
		s.ok = true;
		s.line1 = "Photo  640x480";
	}
	if (!s.ok) {
		clearTop(next);
		s.line1 = m.video ? "Cannot show this video." : "Cannot show this photo.";
		s.line2 = "(damaged or unsupported)";
	} else if (!m.video && uiTopPageLooksBlank(dst)) {
		s.line2 = "(the picture is flat/blank)"; // decoded, but one colour: say so rather than leave it unexplained
	} else if (s.line2.empty()) {
		s.line2 = dateOf(m.name);
	}
	page = next;
	uiTopShowPage(page);
	return s;
}

void drawAlbumButtons(bool hasItems, bool playable) {
	const UiButton list[] = {
		{B_PREV, UI_RECT_PREV, UI_BTN_PREV, UI_BTN_PREV_P, hasItems},
		{B_PLAY, UI_RECT_PLAY, playable ? UI_BTN_PLAY : UI_BTN_PLAY_OFF, UI_BTN_PLAY_P, playable},
		{B_DELETE, UI_RECT_DELETE, UI_BTN_DELETE, UI_BTN_DELETE_P, hasItems},
		{B_NEXT, UI_RECT_NEXT, UI_BTN_NEXT, UI_BTN_NEXT_P, hasItems},
		{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true},
	};
	uiBottomDrawBackground();
	uiShowButtons(list, 5);
}

// Returns 1 = delete, 0 = cancel, -1 = power exit.
int confirmDelete(const MediaItem &m) {
	uiClearButtons();
	uiTextClear();
	uiBottomDrawBackground();
	uiDrawDialogPanel();
	const UiButton list[] = {
		{B_YES, UI_RECT_DLG_YES, UI_DLG_YES, UI_DLG_YES_P, true},
		{B_NO, UI_RECT_DLG_NO, UI_DLG_NO, UI_DLG_NO_P, true},
	};
	uiShowButtons(list, 2);
	uiTextCentred(8, m.video ? "Delete this video?" : "Delete this photo?");
	uiTextCentred(10, m.name.c_str());
	uiTextCentred(12, "This cannot be undone.");

	int result = 0;
	while (true) {
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (appPowerExitRequested()) {
			result = -1;
			break;
		}
		if (down & KEY_LID) {
			appLidSleep(CAM_NONE);
			continue;
		}
		if (touched == B_YES || (down & KEY_A)) {
			result = 1;
			break;
		}
		if (touched == B_NO || (down & KEY_B)) {
			result = 0;
			break;
		}
		swiWaitForVBlank();
	}
	uiClearButtons();
	return result;
}

} // namespace

void galleryList(std::vector<MediaItem> &items, size_t maxCount) {
	items.clear();
	for (int pass = 0; pass < 2; pass++) {
		const bool video = pass == 1;
		DIR *dir = opendir((video ? rec::videoFolder() : photosDirectory()).c_str());
		if (!dir)
			continue;
		while (dirent *ent = readdir(dir)) {
			if (video ? isVideoName(ent->d_name) : isPhotoName(ent->d_name)) {
				MediaItem m;
				m.name = ent->d_name;
				m.video = video;
				items.push_back(m);
				if (items.size() > 4 * MAX_ITEMS)
					break; // absurd folder: stop collecting, the newest are kept below
			}
		}
		closedir(dir);
	}
	// Names are NM_/NV_ + a timestamp: order by the timestamp part
	std::sort(items.begin(), items.end(), [](const MediaItem &a, const MediaItem &b) {
		const int c = strcasecmp(a.name.c_str() + 3, b.name.c_str() + 3);
		return c != 0 ? c < 0 : a.video < b.video;
	});
	if (items.size() > maxCount)
		items.erase(items.begin(), items.end() - maxCount);
}

AlbumExit galleryRun() {
	cameraDeactivateActive(); // the camera is not needed while browsing

	std::vector<MediaItem> items;
	galleryList(items, MAX_ITEMS);

	u16 *scratch = (u16 *)memalign(32, nvid::FRAME_BYTES);
	// The caller faded the top screen to white. Set the viewer's graphics state explicitly (it must not depend on what
	// the live preview left behind), then fade in once the first picture is on its page.
	uiTopViewerInit();
	int page = uiTopShownPage();
	bool fadedIn = false;
	int index = (int)items.size() - 1; // newest first
	bool redraw = true;
	Shown shown;

	uiTopOverlayVisible(false);
	AlbumExit exitCode = AlbumExit::Back;

	while (true) {
		if (redraw) {
			redraw = false;
			uiTextClear();
			if (items.empty()) {
				clearTop(page ^ 1);
				page ^= 1;
				uiTopShowPage(page);
				drawAlbumButtons(false, false);
				uiTextCentred(5, "No photos or videos yet.");
				uiTextCentred(7, "Take one with the shutter!");
				shown = Shown();
			} else {
				if (index >= (int)items.size())
					index = (int)items.size() - 1;
				if (index < 0)
					index = 0;
				shown = scratch ? showItem(items[index], scratch, page) : Shown();
				drawAlbumButtons(true, shown.playable);
				char count[24];
				snprintf(count, sizeof(count), "%d / %d", index + 1, (int)items.size());
				uiBarText(count);
				uiTextCentred(4, items[index].name.c_str());
				uiTextCentred(5, shown.line1.c_str());
				uiTextCentred(6, shown.line2.c_str());
				uiStatus(items[index].video ? "VIDEO" : "PHOTO");
			}
		}

		if (!fadedIn) {
			fadedIn = true;
			uiTopFade(false, 4); // white -> picture (the screen used to stay white here)
		}

		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();

		if (appPowerExitRequested()) {
			exitCode = AlbumExit::PowerExit;
			break;
		}
		if (down & KEY_LID) {
			appLidSleep(CAM_NONE);
			continue;
		}

		int action = touched;
		if (down & KEY_B)
			action = B_BACK;
		else if (down & (KEY_LEFT | KEY_L))
			action = B_PREV;
		else if (down & (KEY_RIGHT | KEY_R))
			action = B_NEXT;
		else if (down & KEY_A)
			action = B_PLAY;
		else if (down & KEY_X)
			action = B_DELETE;
		if (action != touched && action > 0)
			uiPressFeedback(action);

		if (action == B_BACK)
			break;

		if (!items.empty()) {
			const int n = (int)items.size();
			if (action == B_PREV) {
				index = (index + n - 1) % n;
				redraw = true;
			} else if (action == B_NEXT) {
				index = (index + 1) % n;
				redraw = true;
			} else if (action == B_PLAY && shown.playable && items[index].video) {
				const PlayerExit r = playerRun(itemPath(items[index]), items[index].name.substr(3, 8));
				if (r == PlayerExit::PowerExit) {
					exitCode = AlbumExit::PowerExit;
					break;
				}
				uiTopOverlayVisible(false);
				page = uiTopShownPage(); // the player flipped pages itself
				redraw = true;
			} else if (action == B_DELETE) {
				const int c = confirmDelete(items[index]);
				if (c < 0) {
					exitCode = AlbumExit::PowerExit;
					break;
				}
				if (c == 1) {
					const bool ok = items[index].video ? (remove(itemPath(items[index]).c_str()) == 0) : photosDelete(items[index].name);
					if (ok) {
						items.erase(items.begin() + index);
						if (index >= (int)items.size())
							index = (int)items.size() - 1;
					} else {
						uiTextClear();
						uiTextCentred(9, "Could not delete the file.");
						appWaitFrames(90);
					}
				}
				redraw = true;
			}
		}

		swiWaitForVBlank();
	}

	free(scratch);
	uiClearButtons();
	uiTopFade(true, 4); // back to white; the caller restores the preview and fades in
	uiTopOverlayVisible(true);
	return exitCode;
}
