#include "settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "app.h"
#include "audioPlayer.h"
#include "audioRecorder.h"
#include "audiofmt.h"
#include "camera.h"
#include "camsettings.h"
#include "fpsutil.h"
#include "videofmt.h"
#include "msclock.h"
#include "reclog.h"
#include "sdbench.h"
#include "ui.h"
#include "video.h"

namespace camset {

namespace {

enum { B_ROW0 = 100, B_ROW1, B_ROW2, B_ROW3, B_ROW4, B_BACK_ID = 110 };

constexpr int ROW_X = 20, ROW_W = 216, ROW_H = 22, ROW_Y0 = 32, ROW_PITCH = 24;

int rowImage(int i) { return UI_ROW0 + i; }
// console row (8 px) that is vertically centred in button row i
int rowTextRow(int i) { return (ROW_Y0 + ROW_PITCH * i + ROW_H / 2) / 8; }

// Buttons: `rows` ROW images plus Back. Returns the id array through the buttons list given to uiShowButtons.
void showRows(int rows, const char *const *labels, const char *barText) {
	uiClearButtons();
	uiBottomDrawBackground();
	UiButton list[8];
	int n = 0;
	for (int i = 0; i < rows && n < 6; i++)
		list[n++] = {B_ROW0 + i, (short)ROW_X, (short)(ROW_Y0 + ROW_PITCH * i), (short)ROW_W, (short)ROW_H, rowImage(i), -1, true};
	list[n++] = {B_BACK_ID, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true};
	uiShowButtons(list, n);
	uiTextClear();
	for (int i = 0; i < rows; i++)
		uiTextCentred(rowTextRow(i), labels[i]);
	uiBarText(barText);
}

// One frame of input: returns a pressed button id (touch or key mapping), 0 for nothing, -1 for power exit.
// Keys: UP/DOWN move the selection (shown with > <), A activates, B = back.
struct Nav {
	int sel = 0;
	int rows = 1;
};

int pollInput(Nav &nav, bool &moved) {
	scanKeys();
	const u32 down = keysDown(), up = keysUp();
	const int touched = uiHandleInput(down, up);
	uiTick();
	if (appPowerExitRequested())
		return -1;
	moved = false;
	if (touched >= 0)
		return touched;
	if (down & KEY_B)
		return B_BACK_ID;
	if (down & KEY_DOWN) {
		nav.sel = (nav.sel + 1) % nav.rows;
		moved = true;
	} else if (down & KEY_UP) {
		nav.sel = (nav.sel + nav.rows - 1) % nav.rows;
		moved = true;
	} else if (down & KEY_A) {
		return B_ROW0 + nav.sel;
	}
	return 0;
}

void marker(const Nav &nav) {
	for (int i = 0; i < nav.rows; i++) {
		uiTextAt(3, rowTextRow(i), i == nav.sel ? ">" : " ");
		uiTextAt(28, rowTextRow(i), i == nav.sel ? "<" : " ");
	}
}

reclog::RecLog viewLog() {
	reclog::RecLog v = rec::lastRecLog();
	if (v.result[0])
		return v;
	static char text[2600];
	FILE *f = fopen((rec::videoFolder() + "/last-recording.txt").c_str(), "rb");
	if (f) {
		const size_t n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
		text[n] = 0;
		reclog::parse(text, v);
	}
	return v;
}

const char *fpsWord(int fps) {
	switch (fps) {
		case 15: return "Smooth";
		case 20: return "High";
		case 30: return "Max";
		default: return "Safe";
	}
}

// ---------------------------------------------------------------- generic text dialog

struct Page {
	char line[10][32];
	int count = 0;
	void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

void Page::add(const char *fmt, ...) {
	if (count >= 10)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line[count], sizeof(line[count]), fmt, ap);
	va_end(ap);
	count++;
}

void drawDialog(const char *title, const Page &p, const char *footer) {
	uiClearButtons();
	const UiButton ok[] = {{B_BACK_ID, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true}};
	uiTextClear();
	uiBottomDrawBackground();
	uiDrawDialogPanel();
	uiShowButtons(ok, 1);
	uiTextCentred(6, title);
	for (int i = 0; i < p.count && i < 7; i++)
		uiTextAt(4, 8 + i, p.line[i]);
	if (footer && footer[0])
		uiTextAt(4, 15, footer);
}

// ---------------------------------------------------------------- recording info (long)

const char *micWord(const reclog::RecLog &v) {
	if (!strcmp(v.mic_class, "MIC_VALID_DATA"))
		return "OK";
	if (!strcmp(v.mic_class, "MIC_NO_CALLBACKS"))
		return "No data";
	if (!strcmp(v.mic_class, "MIC_CALLBACKS_BUT_ZERO_DATA"))
		return "Silent";
	if (!strcmp(v.mic_class, "MIC_INIT_FAILED"))
		return "Failed";
	return "?";
}

void buildInfoPage(int page, const reclog::RecLog &v, Page &p, const char *&title) {
	const bool have = v.result[0] != 0;
	const u32 afps = reclog::fpsX100(v.captured_frames, v.duration_ms);
	if (!have) {
		title = "Recording Info";
		p.add("No video yet");
		return;
	}
	if (page == 0) {
		title = "Video";
		p.add("Requested %u FPS", (unsigned)v.requested_fps);
		p.add("Actual %u.%02u FPS", (unsigned)(afps / 100), (unsigned)(afps % 100));
		p.add("Frames %u  Dropped %u", (unsigned)v.captured_frames, (unsigned)v.dropped_frames);
		p.add("Camera gave %u frames", (unsigned)v.camera_frames_seen);
		p.add("Buffers used %u of %u", (unsigned)v.buffer_peak, (unsigned)v.buffer_slots);
		p.add("Length %u s", (unsigned)((v.duration_ms + 500) / 1000));
		p.add("%s  %s", v.result, v.recording_format);
	} else if (page == 1) {
		title = "SD card";
		const u32 mbs = reclog::mbPerSecX100(v.sd_bytes, v.sd_total_write_ms);
		p.add("Writes %u", (unsigned)v.sd_write_count);
		p.add("Average %u ms", (unsigned)v.sd_avg_write_ms);
		p.add("Slowest %u ms", (unsigned)v.sd_max_write_ms);
		p.add("Over 250 ms: %u", (unsigned)v.sd_slow_writes_250ms);
		p.add("Speed %u.%02u MB/s", (unsigned)(mbs / 100), (unsigned)(mbs % 100));
		p.add("Stopped: %s", v.stop_reason);
	} else {
		title = "Microphone";
		p.add("Result: %s", micWord(v));
		p.add("Callbacks %u", (unsigned)v.audio_callbacks);
		p.add("Samples %u", (unsigned)v.audio_samples);
		p.add("Peak %u", (unsigned)v.audio_peak);
		p.add("Chunks %u  Failed %u", (unsigned)v.audio_chunks, (unsigned)v.audio_failed);
	}
}

// ---------------------------------------------------------------- SD speed test

bool benchCancel() {
	scanKeys();
	return (keysHeld() & KEY_B) != 0;
}

void benchProgress(const char *name, int index, int total) {
	char line[32];
	uiTextAt(4, 11, "                          ");
	snprintf(line, sizeof(line), "%s  (%d/%d)", name, index + 1, total);
	uiTextAt(4, 11, line);
}

sdbench::Report bench;
bool haveBench = false;

void showBench(int page) {
	Page p;
	const char *title = "SD speed test";
	const int perPage = 6;
	const int pages = 1 + (bench.count + perPage - 1) / perPage;
	if (page == 0) {
		p.add("%s", bench.wroteReport ? "Report file: SAVED" : "REPORT NOT SAVED");
		if (bench.wroteReport)
			p.add("_nds/nerdMod/");
		if (bench.wroteReport)
			p.add("sd-benchmark.txt");
		else
			p.add("%s", bench.writeError[0] ? bench.writeError : "unknown error");
		const sdbenchcalc::Result *g98 = nullptr, *mic = nullptr, *rw = nullptr;
		for (int i = 0; i < bench.count; i++) {
			if (!strcmp(bench.tests[i].name, "g98K")) g98 = &bench.tests[i];
			if (!strcmp(bench.tests[i].name, "g98K+mic")) mic = &bench.tests[i];
			if (!strcmp(bench.tests[i].name, "rw98K")) rw = &bench.tests[i];
		}
		auto mb = [](const sdbenchcalc::Result *r, char *out, size_t n) {
			if (!r || !r->ok) { snprintf(out, n, "n/a"); return; }
			const u32 m = sdbench::mbPerSecX100(*r);
			snprintf(out, n, "%u.%02u MB/s", (unsigned)(m / 100), (unsigned)(m % 100));
		};
		char a[16];
		mb(g98, a, sizeof a); p.add("Write  %s", a);
		mb(rw, a, sizeof a); p.add("Rewrite %s", a);
		mb(mic, a, sizeof a); p.add("With mic %s", a);
		if (bench.cancelled)
			p.add("(cancelled)");
	} else {
		const int first = (page - 1) * perPage;
		for (int i = first; i < bench.count && i < first + perPage; i++) {
			const sdbenchcalc::Result &r = bench.tests[i];
			const u32 m = sdbench::mbPerSecX100(r);
			if (r.ok)
				p.add("%-9s %u.%02uMB/s %ums", r.name, (unsigned)(m / 100), (unsigned)(m % 100), (unsigned)(r.p95Us / 1000));
			else
				p.add("%-9s FAILED", r.name);
		}
		title = "SD tests (p95 ms)";
	}
	char foot[32];
	snprintf(foot, sizeof(foot), "A: page %d/%d", page + 1, pages);
	drawDialog(title, p, foot);
}

Exit sdSpeedTest() {
	uiClearButtons();
	uiTextClear();
	uiBottomDrawBackground();
	uiDrawDialogPanel();
	uiTextCentred(6, "SD speed test");
	uiTextCentred(8, "Please wait (B stops)");
	uiTextAt(4, 13, "Report:");
	uiTextAt(4, 14, "_nds/nerdMod/sd-benchmark.txt");
	sdbench::run(bench, benchProgress, benchCancel);
	haveBench = true;
	int page = 0;
	const int perPage = 6;
	const int pages = 1 + (bench.count + perPage - 1) / perPage;
	bool redraw = true;
	Nav nav;
	while (true) {
		if (redraw) {
			showBench(page);
			redraw = false;
		}
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (appPowerExitRequested())
			return Exit::Power;
		if ((down & KEY_B) || touched == B_BACK_ID)
			return Exit::Back;
		if (down & (KEY_A | KEY_RIGHT)) {
			page = (page + 1) % pages;
			redraw = true;
		} else if (down & KEY_LEFT) {
			page = (page + pages - 1) % pages;
			redraw = true;
		}
		swiWaitForVBlank();
	}
}

// ---------------------------------------------------------------- audio test

Exit audioTest() {
	const char *labels[2] = {"PLAY TEST TONE", "MIC TEST (3 SEC)"};
	Nav nav;
	nav.rows = 2;
	showRows(2, labels, "AUDIO TEST");
	marker(nav);
	char status1[32] = "", status2[32] = "", status3[32] = "";
	auto drawStatus = [&]() {
		uiTextAt(2, 12, "                              ");
		uiTextAt(2, 13, "                              ");
		uiTextAt(2, 14, "                              ");
		uiTextAt(2, 12, status1);
		uiTextAt(2, 13, status2);
		uiTextAt(2, 14, status3);
	};
	while (true) {
		bool moved;
		const int act = pollInput(nav, moved);
		if (act < 0)
			return Exit::Power;
		if (moved)
			marker(nav);
		if (act == B_BACK_ID)
			return Exit::Back;
		if (act == B_ROW0) {
			if (audioPlay::startTone()) {
				snprintf(status1, sizeof status1, "TEST TONE PLAYING");
				snprintf(status2, sizeof status2, "1 kHz, channel %d", audioPlay::channelId());
				snprintf(status3, sizeof status3, "Do you hear a beep?");
				drawStatus();
				for (int i = 0; i < 100 && !appPowerExitRequested(); i++)
					swiWaitForVBlank();
				audioPlay::stop();
				snprintf(status1, sizeof status1, "Tone finished");
			} else {
				snprintf(status1, sizeof status1, "TONE FAILED: %s", audioPlay::lastError());
				snprintf(status2, sizeof status2, "Master volume / channel");
				status3[0] = 0;
			}
			drawStatus();
		} else if (act == B_ROW1) {
			snprintf(status1, sizeof status1, "Recording... speak now");
			status2[0] = status3[0] = 0;
			drawStatus();
			const bool started = audioRec::start();
			const u32 t0 = msclock::ticks();
			msclock::start();
			const u32 begin = msclock::ticks();
			(void)t0;
			u32 peak = 0;
			while (started && msclock::toMs(msclock::ticks() - begin) < 3000 && !appPowerExitRequested()) {
				u8 tmp[2048];
				while (audioRec::available() >= sizeof tmp)
					audioRec::read(tmp, sizeof tmp);
				swiWaitForVBlank();
			}
			const u32 elapsed = msclock::toMs(msclock::ticks() - begin);
			msclock::stop();
			peak = audioRec::peakSample();
			const u32 samples = audioRec::sampleCount(), cb = audioRec::callbacks();
			const int init = audioRec::startStatus();
			audioRec::stop();
			const u32 expect = (u32)((u64)elapsed * audioRec::sampleRate() / 1000u);
			if (!started) {
				snprintf(status1, sizeof status1, "MIC FAILED (init %d)", init);
				status2[0] = status3[0] = 0;
			} else {
				snprintf(status1, sizeof status1, "%u of ~%u samples", (unsigned)samples, (unsigned)expect);
				snprintf(status2, sizeof status2, "Callbacks %u  Peak %u", (unsigned)cb, (unsigned)peak);
				snprintf(status3, sizeof status3, "%s", audioRec::pathName());
			}
			drawStatus();
		}
		swiWaitForVBlank();
	}
}

// ---------------------------------------------------------------- camera info

void buildCameraPage(Page &p) {
	p.add("DSi hardware: %s", cameraHardwareAccessible() ? "yes" : "NO");
	p.add("Inner camera: %s", cameraAvailable(CAM_INNER) ? "ready" : "none");
	p.add("Outer camera: %s", cameraAvailable(CAM_OUTER) ? "ready" : "none");
	p.add("Video: %d FPS %s", camsettings::videoFps(), fpsWord(camsettings::videoFps()));
	p.add("Quality: %s", vfmt::qualityName(camsettings::videoQuality()));
	p.add("Format: %s", rec::formatName());
	p.add("Mic: %s", audioRec::pathName());
}

} // namespace

// ---------------------------------------------------------------- public

Exit chooseFps(bool *changed) {
	if (changed)
		*changed = false;
	char text[4][28];
	const char *labels[4];
	Nav nav;
	nav.rows = 4;
	for (int i = 0; i < 4; i++) {
		const int f = fpsutil::RATES[i];
		snprintf(text[i], sizeof text[i], "%d FPS  %s", f, fpsWord(f));
		labels[i] = text[i];
		if (f == rec::fps())
			nav.sel = i;
	}
	showRows(4, labels, "VIDEO FRAME RATE");
	marker(nav);
	char hint[32];
	snprintf(hint, sizeof hint, "~%d MB/min at %d FPS", fpsutil::mbPerMinute(fpsutil::RATES[nav.sel]), fpsutil::RATES[nav.sel]);
	uiTextCentred(18, hint);
	while (true) {
		bool moved;
		const int act = pollInput(nav, moved);
		if (act < 0)
			return Exit::Power;
		if (moved) {
			marker(nav);
			snprintf(hint, sizeof hint, "~%d MB/min at %d FPS", fpsutil::mbPerMinute(fpsutil::RATES[nav.sel]), fpsutil::RATES[nav.sel]);
			uiTextCentred(18, hint);
		}
		if (act == B_BACK_ID)
			return Exit::Back;
		if (act >= B_ROW0 && act < B_ROW0 + 4) {
			const int fps = fpsutil::RATES[act - B_ROW0];
			if (changed)
				*changed = fps != rec::fps();
			rec::setFps(fps);
			camsettings::setVideoFps(fps);
			return Exit::Back;
		}
		swiWaitForVBlank();
	}
}

Exit chooseQuality() {
	char text[3][28];
	const char *labels[3];
	Nav nav;
	nav.rows = vfmt::QUALITY_COUNT;
	for (int i = 0; i < vfmt::QUALITY_COUNT; i++) {
		snprintf(text[i], sizeof text[i], "%s", vfmt::qualityName(i));
		labels[i] = text[i];
	}
	nav.sel = vfmt::sanitizeQuality(camsettings::videoQuality());
	showRows(vfmt::QUALITY_COUNT, labels, "VIDEO QUALITY");
	marker(nav);
	uiTextCentred(18, vfmt::qualityHint(nav.sel));
	while (true) {
		bool moved;
		const int act = pollInput(nav, moved);
		if (act < 0)
			return Exit::Power;
		if (moved) {
			marker(nav);
			uiTextAt(2, 18, "                              ");
			uiTextCentred(18, vfmt::qualityHint(nav.sel));
		}
		if (act == B_BACK_ID)
			return Exit::Back;
		if (act >= B_ROW0 && act < B_ROW0 + vfmt::QUALITY_COUNT) {
			rec::setQuality(act - B_ROW0);
			camsettings::setVideoQuality(act - B_ROW0);
			return Exit::Back;
		}
		swiWaitForVBlank();
	}
}

Exit recordingInfo(int page) {
	const reclog::RecLog v = viewLog();
	const int pages = v.result[0] ? 4 : 2; // the last page is always the camera information
	bool redraw = true;
	if (page >= pages)
		page = 0;
	while (true) {
		if (redraw) {
			Page p;
			const char *title = "Recording Info";
			if (page == pages - 1) {
				title = "Camera info";
				buildCameraPage(p);
			} else {
				buildInfoPage(page, v, p, title);
			}
			char foot[32];
			snprintf(foot, sizeof(foot), "A: page %d/%d", page + 1, pages);
			drawDialog(title, p, foot);
			redraw = false;
		}
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (appPowerExitRequested())
			return Exit::Power;
		if ((down & KEY_B) || touched == B_BACK_ID)
			return Exit::Back;
		if (down & (KEY_A | KEY_RIGHT)) {
			page = (page + 1) % pages;
			redraw = true;
		} else if (down & KEY_LEFT) {
			page = (page + pages - 1) % pages;
			redraw = true;
		}
		swiWaitForVBlank();
	}
}

Exit recordingSummary() {
	const reclog::RecLog v = viewLog();
	Page p;
	if (!v.result[0]) {
		p.add("No video yet");
	} else {
		const u32 afps = reclog::fpsX100(v.captured_frames, v.duration_ms);
		const u32 mbs = reclog::mbPerSecX100(v.sd_bytes, v.sd_total_write_ms);
		p.add("Requested  %u FPS", (unsigned)v.requested_fps);
		p.add("Actual     %u.%02u FPS", (unsigned)(afps / 100), (unsigned)(afps % 100));
		p.add("Dropped    %u", (unsigned)v.dropped_frames);
		p.add("SD speed   %u.%02u MB/s", (unsigned)(mbs / 100), (unsigned)(mbs % 100));
		p.add("Mic        %s", micWord(v));
		p.add("Format     %s", v.recording_format);
	}
	drawDialog("Video saved", p, "A: details");
	while (true) {
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (appPowerExitRequested())
			return Exit::Power;
		if ((down & KEY_B) || touched == B_BACK_ID)
			return Exit::Back;
		if (down & KEY_A)
			return recordingInfo(0);
		swiWaitForVBlank();
	}
}

Exit run() {
	Nav nav;
	nav.rows = 5;
	char fpsLabel[28], qualLabel[28];
	const char *labels[5] = {fpsLabel, "Recording Info", "SD Speed Test", "Audio Test", qualLabel};
	bool redraw = true;
	while (true) {
		if (redraw) {
			snprintf(fpsLabel, sizeof fpsLabel, "Video FPS: %d", rec::fps());
			snprintf(qualLabel, sizeof qualLabel, "Video Quality: %s", vfmt::qualityName(rec::quality()));
			showRows(5, labels, "CAMERA SETTINGS");
			marker(nav);
			redraw = false;
		}
		bool moved;
		const int act = pollInput(nav, moved);
		if (act < 0)
			return Exit::Power;
		if (moved)
			marker(nav);
		if (act == B_BACK_ID)
			return Exit::Back;
		Exit e = Exit::Back;
		bool ran = true;
		switch (act) {
			case B_ROW0: e = chooseFps(nullptr); break;
			case B_ROW1: e = recordingInfo(0); break;
			case B_ROW2: e = sdSpeedTest(); break;
			case B_ROW3: e = audioTest(); break;
			case B_ROW4: e = chooseQuality(); break;
			default: ran = false; break;
		}
		if (ran) {
			if (e == Exit::Power)
				return Exit::Power;
			redraw = true;
		}
		swiWaitForVBlank();
	}
}

} // namespace camset
