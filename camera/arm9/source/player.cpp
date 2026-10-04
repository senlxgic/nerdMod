#include "player.h"

#include <malloc.h>
#include <stdio.h>

#include "app.h"
#include "msclock.h"
#include "ui.h"
#include "videoContainer.h"

namespace {

enum { B_PREV = 1, B_PLAY, B_NEXT, B_BACK };

constexpr u32 SEEK_MS = 5000;

void fmtTime(char *out, size_t n, u32 ms) {
	const u32 s = ms / 1000;
	snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void drawButtons(bool playing, bool canPlay) {
	const UiButton list[] = {
		{B_PREV, UI_RECT_PREV, UI_BTN_PREV, UI_BTN_PREV_P, true},
		{B_PLAY, UI_RECT_PLAY, playing ? UI_BTN_PAUSE : (canPlay ? UI_BTN_PLAY : UI_BTN_PLAY_OFF), playing ? UI_BTN_PAUSE_P : UI_BTN_PLAY_P, canPlay},
		{B_NEXT, UI_RECT_NEXT, UI_BTN_NEXT, UI_BTN_NEXT_P, true},
		{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true},
	};
	uiBottomDrawBackground();
	uiShowButtons(list, 4);
}

} // namespace

PlayerExit playerRun(const std::string &path, const std::string &title) {
	nvid::Reader reader;
	u16 *frame = (u16 *)memalign(32, nvid::FRAME_BYTES);
	uiTextClear();
	uiTopOverlayVisible(false);

	PlayerExit exitCode = PlayerExit::Back;
	if (!frame || !reader.open(path)) {
		free(frame);
		uiTextClear();
		uiBottomDrawBackground();
		uiTextCentred(8, "Cannot play this video.");
		uiTextCentred(10, "(damaged or unsupported)");
		for (int i = 0; i < 120; i++) {
			scanKeys();
			if ((keysDown() & (KEY_A | KEY_B | KEY_TOUCH)) || appPowerExitRequested())
				break;
			swiWaitForVBlank();
		}
		uiTopOverlayVisible(true);
		return appPowerExitRequested() ? PlayerExit::PowerExit : PlayerExit::Back;
	}

	const u32 duration = reader.durationMs();
	char total[16];
	fmtTime(total, sizeof(total), duration);

	int page = 0;
	bool playing = true;
	bool atEnd = false;
	u32 position = 0;		// playback position in ms (valid while paused)
	u32 baseTicks = 0;		// clock value at position 0 (valid while playing)
	bool haveFrame = false; // `frame` holds a decoded frame that is not shown yet
	u32 frameTime = 0;
	msclock::start();

	auto nowPos = [&]() -> u32 { return msclock::toMs(msclock::ticks() - baseTicks); };
	auto currentPos = [&]() -> u32 { return playing ? nowPos() : position; };
	auto startAt = [&](u32 ms) {
		position = ms;
		baseTicks = msclock::ticks() - (u32)(((u64)ms * 33513982ull) / 1024000ull);
	};
	auto updateText = [&](u32 ms) {
		char cur[16], line[32];
		fmtTime(cur, sizeof(cur), ms > duration ? duration : ms);
		snprintf(line, sizeof(line), "%s / %s", cur, total);
		uiStatus(line);
	};
	auto show = [&]() {
		DC_FlushRange(frame, nvid::FRAME_BYTES); // written by the CPU, copied by DMA
		dmaCopyHalfWords(3, frame, uiTopPage(page ^ 1), nvid::FRAME_BYTES);
		page ^= 1;
		uiTopShowPage(page);
		haveFrame = false;
	};
	// Reads the next video frame into `frame` (audio is skipped, frames that are far too late are skipped without
	// reading their pixels). Returns false at the end of the data.
	auto fetch = [&](bool allowSkip) -> bool {
		nvid::Reader::Chunk c;
		for (int guard = 0; guard < 6; guard++) {
			if (!reader.nextChunk(c))
				return false;
			if (c.fourcc == nvid::CHUNK_VIDEO && c.size == nvid::FRAME_BYTES) {
				if (allowSkip && nowPos() > c.timeMs + 150) {
					if (!reader.skipPayload(c.size))
						return false;
					continue;
				}
				if (!reader.readPayload(frame, c.size))
					return false;
				frameTime = c.timeMs;
				haveFrame = true;
				return true;
			}
			if (!reader.skipPayload(c.size))
				return false;
		}
		return true; // only audio so far: try again next iteration
	};

	uiTextClear();
	drawButtons(true, true);
	uiBarText(title.c_str());
	reader.rewind();
	startAt(0);
	updateText(0);
	u32 lastShownSecond = 0xFFFFFFFF;

	while (true) {
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();

		if (appPowerExitRequested()) {
			exitCode = PlayerExit::PowerExit;
			break;
		}
		if (down & KEY_LID) {
			const u32 pos = currentPos();
			appLidSleep(CAM_NONE);
			if (playing)
				startAt(pos);
			continue;
		}

		int action = touched;
		if (down & KEY_B)
			action = B_BACK;
		else if (down & KEY_A)
			action = B_PLAY;
		else if (down & (KEY_LEFT | KEY_L))
			action = B_PREV;
		else if (down & (KEY_RIGHT | KEY_R))
			action = B_NEXT;
		if (action != touched && action > 0)
			uiPressFeedback(action);

		if (action == B_BACK)
			break;

		if (action == B_PLAY) {
			if (atEnd) {
				reader.rewind();
				atEnd = false;
				haveFrame = false;
				playing = true;
				startAt(0);
			} else if (playing) {
				position = nowPos();
				playing = false;
			} else {
				startAt(position);
				playing = true;
			}
			uiSetButtonImages(B_PLAY, playing ? UI_BTN_PAUSE : UI_BTN_PLAY, playing ? UI_BTN_PAUSE_P : UI_BTN_PLAY_P);
		} else if (action == B_PREV || action == B_NEXT) {
			const u32 pos = currentPos();
			u32 target = (action == B_PREV) ? (pos > SEEK_MS ? pos - SEEK_MS : 0) : pos + SEEK_MS;
			if (target >= duration)
				target = duration > 300 ? duration - 300 : 0;
			reader.seekToTime(target);
			atEnd = false;
			haveFrame = false;
			startAt(target);
			if (!playing) {
				// paused: show the picture at the new position, then stay there
				if (fetch(false) && haveFrame)
					show();
				reader.seekToTime(target);
			}
			updateText(target);
			lastShownSecond = target / 1000;
		}

		if (playing && !atEnd) {
			if (!haveFrame && !fetch(true)) {
				atEnd = true;
				playing = false;
				position = duration;
				uiSetButtonImages(B_PLAY, UI_BTN_PLAY, UI_BTN_PLAY_P);
			}
			if (haveFrame && nowPos() + 5 >= frameTime)
				show();
		}

		const u32 shownMs = currentPos();
		if (shownMs / 1000 != lastShownSecond) {
			lastShownSecond = shownMs / 1000;
			updateText(shownMs);
		}

		swiWaitForVBlank();
	}

	msclock::stop();
	reader.close();
	free(frame);
	uiTopOverlayVisible(true);
	return exitCode;
}
