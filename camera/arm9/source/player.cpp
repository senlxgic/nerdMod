#include "player.h"

#include <malloc.h>
#include <stdio.h>

#include "app.h"
#include "audioPlayer.h"
#include "audiofmt.h"
#include "msclock.h"
#include "ui.h"
#include "videoContainer.h"

namespace {

enum { B_PREV = 1, B_PLAY, B_NEXT, B_BACK };

constexpr u32 SEEK_MS = 5000;
constexpr u32 ABUF_BYTES = 32768; // largest audio chunk accepted
constexpr u32 AUDIO_LOOKBACK_MS = 800; // audio chunks are written up to ~0.75 s ahead of their video

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
	u8 *abuf = (u8 *)memalign(32, ABUF_BYTES);
	uiTextClear();
	uiTopOverlayVisible(false);

	PlayerExit exitCode = PlayerExit::Back;
	if (!frame || !abuf || !reader.open(path)) {
		free(frame);
		free(abuf);
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

	int page = uiTopShownPage(); // the page the Album left visible: frames go to the other one
	bool playing = true;
	bool atEnd = false;
	u32 position = 0;		// playback position in ms (valid while paused)
	u32 baseTicks = 0;		// clock value at position 0 (valid while playing)
	bool haveFrame = false; // `frame` holds a decoded frame that is not shown yet
	u32 frameTime = 0;
	msclock::start();

	// With sound the audio clock is the master; without it the millisecond clock.
	auto nowPos = [&]() -> u32 { return audioPlay::active() ? audioPlay::positionMs() : msclock::toMs(msclock::ticks() - baseTicks); };
	auto currentPos = [&]() -> u32 { return playing ? nowPos() : position; };
	auto startAt = [&](u32 ms) {
		position = ms;
		baseTicks = msclock::ticks() - (u32)(((u64)ms * 33513982ull) / 1024000ull);
	};
	// Positions the file and both clocks at `ms` and starts the audio (when the file has any and we are playing).
	auto beginAt = [&](u32 ms, bool play) {
		audioPlay::stop();
		haveFrame = false;
		const bool withAudio = play && reader.hasAudio();
		reader.seekToTime(withAudio && ms > AUDIO_LOOKBACK_MS ? ms - AUDIO_LOOKBACK_MS : ms);
		startAt(ms);
		if (withAudio)
			audioPlay::start(ms); // false = no sound, the video clock keeps time
	};
	audioPlay::resetStats();
	auto updateText = [&](u32 ms) {
		char cur[16], line[40];
		fmtTime(cur, sizeof(cur), ms > duration ? duration : ms);
		// the audio state of the player, kept apart from what the recorder stored (see audiofmt.h)
		const audiofmt::PlayClass pc = audiofmt::classifyPlayback(reader.hasAudio(), audioPlay::startAttempted(), audioPlay::startSucceeded(), audioPlay::lateChunks(), audioPlay::chunksFed());
		const char *tag = pc == audiofmt::FILE_HAS_NO_AUDIO ? "NO SOUND" : pc == audiofmt::AUDIO_CHANNEL_START_FAILED ? "CH FAIL" : pc == audiofmt::AUDIO_UNDERRUN ? "SND LATE" : pc == audiofmt::AUDIO_PLAYING ? "SND OK" : "SND";
		snprintf(line, sizeof(line), "%s / %s  %s", cur, total, tag);
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
		for (int guard = 0; guard < 12; guard++) {
			if (!reader.nextChunk(c))
				return false;
			if (c.fourcc == nvid::CHUNK_AUDIO && audioPlay::active() && c.size <= ABUF_BYTES) {
				if (!reader.readPayload(abuf, c.size))
					return false;
				audioPlay::feed(c.timeMs, abuf, c.size);
				continue;
			}
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
	beginAt(0, true);
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
				beginAt(pos, true);
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
				atEnd = false;
				playing = true;
				beginAt(0, true);
			} else if (playing) {
				position = nowPos();
				playing = false;
				audioPlay::stop();
			} else {
				playing = true;
				beginAt(position, true);
			}
			uiSetButtonImages(B_PLAY, playing ? UI_BTN_PAUSE : UI_BTN_PLAY, playing ? UI_BTN_PAUSE_P : UI_BTN_PLAY_P);
		} else if (action == B_PREV || action == B_NEXT) {
			const u32 pos = currentPos();
			u32 target = (action == B_PREV) ? (pos > SEEK_MS ? pos - SEEK_MS : 0) : pos + SEEK_MS;
			if (target >= duration)
				target = duration > 300 ? duration - 300 : 0;
			atEnd = false;
			beginAt(target, playing);
			if (!playing) {
				// paused: show the picture at the new position, then stay there
				if (fetch(false) && haveFrame)
					show();
				reader.seekToTime(target);
			}
			updateText(target);
			lastShownSecond = target / 1000;
		}

		audioPlay::update();
		if (playing && !atEnd) {
			if (!haveFrame && !fetch(true)) {
				atEnd = true;
				playing = false;
				position = duration;
				audioPlay::stop();
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

	audioPlay::stop();
	msclock::stop();
	reader.close();
	free(frame);
	free(abuf);
	uiTopOverlayVisible(true);
	return exitCode;
}
