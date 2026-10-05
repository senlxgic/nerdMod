/*
	nerdMod Music

	A small local music player launched from the home screen like any other app. It plays MP3 (minimp3, public domain / CC0)
	and PCM WAV files from sd:/Music and sd:/_nds/nerdMod/music, streaming them through two looping hardware sound channels.
	Library scan results are cached in sd:/_nds/nerdMod/cache/music-index.txt. It reuses the Camera's user-interface layer
	(ui.cpp), exactly like the Photos app does. Music only plays inside this app: leaving it stops playback.

	Player:   A play/pause, L/R previous/next, Left/Right seek 10 s, Up/Down volume, X shuffle, Y repeat,
	          SELECT library, B back to the menu; touch: the buttons, the progress bar, the [-] [+] volume keys
	Library:  Up/Down (L/R page), A play, Y rescan, B back to the player (or the menu when nothing plays)
*/

#include <nds.h>

#include <fat.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "msclock.h"
#include "musiclib.h"
#include "musicmath.h"
#include "musicplayer.h"
#include "ui.h"

#include "common/nds_loader_arm9.h"
#include "common/nmfont.h"
#include "common/nmmusic.h"
#include "common/systemdetails.h"
#include "common/twlmenusettings.h"
#include "myDSiMode.h"

// Symbols the shared TWiLight sources expect from the application
bool fadeType = false;
bool controlTopBright = true;
bool controlBottomBright = true;
bool useTwlCfg = false;

namespace {

bool powerExit() { return fifoCheckValue32(FIFO_USER_01); }

std::string root() { return sys().isRunFromSD() ? "sd:" : "fat:"; }
std::string nerdDir() { return root() + "/_nds/nerdMod"; }

[[noreturn]] void stopForever() {
	while (true)
		swiWaitForVBlank();
}

[[noreturn]] void returnToMenu() {
	mplay::stop();
	uiTopFade(true, 6);
	setBrightness(3, 16);
	for (int i = 0; i < 4; i++)
		swiWaitForVBlank();

	const bool sd = sys().isRunFromSD();
	const char *menu = sd ? "sd:/_nds/TWiLightMenu/dsimenu.srldr" : "fat:/_nds/TWiLightMenu/dsimenu.srldr";
	switch (ms().theme) {
		case TWLSettings::EThemeDSi:
		case TWLSettings::EThemeHBL:
		case TWLSettings::EThemeSaturn:
			if (!ms().showSelectMenu)
				menu = sd ? "sd:/_nds/TWiLightMenu/mainmenu.srldr" : "fat:/_nds/TWiLightMenu/mainmenu.srldr";
			break;
		case TWLSettings::EThemeR4:
		case TWLSettings::EThemeGBC:
			menu = sd ? "sd:/_nds/TWiLightMenu/r4menu.srldr" : "fat:/_nds/TWiLightMenu/r4menu.srldr";
			break;
		case TWLSettings::EThemeWood:
			menu = sd ? "sd:/_nds/TWiLightMenu/mainmenu.srldr" : "fat:/_nds/TWiLightMenu/mainmenu.srldr";
			break;
		default:
			break;
	}
	std::vector<const char *> argv;
	argv.push_back(menu);
	runNdsFile(menu, argv.size(), &argv[0], sys().isRunFromSD(), true, false, false, true, true, false, -1);
	runNdsFile(sys().isRunFromSD() ? "sd:/boot.nds" : "fat:/boot.nds", 0, NULL, sys().isRunFromSD(), true, true, false, true, true, false, -1);

	setBrightness(3, 0);
	uiTextClear();
	uiTextCentred(8, "Could not return to menu.");
	uiTextCentred(10, "Hold the power button.");
	stopForever();
}

void lidSleep() {
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
}

// ---- state -------------------------------------------------------------------------------------------------
enum { B_BACK = 1, B_PREV, B_PLAY, B_NEXT, B_LIB };
enum class Repeat : u8 { Off, All, One };
enum class Mode : u8 { Player, Library };

nmmusic::Track tracks[nmmusic::MAX_TRACKS];
int trackCount = 0;
int cur = -1;		// index of the loaded track (-1 = none)
int sel = 0, scroll = 0;
bool shuffle = false;
Repeat repeat = Repeat::All;
uint32_t rng = 12345;
int playedInRun = 0;
Mode mode = Mode::Library;
bool bottomDirty = true, topDirty = true;
char note[40] = ""; // transient message on the status plate
int noteFrames = 0;
int topPage = 0;
int vuL = 0, vuR = 0;
uint32_t lastSecond = 0xFFFFFFFF;

// ---- preferences (sd:/_nds/nerdMod/music.ini) ---------------------------------------------------------------
void loadPrefs() {
	FILE *f = fopen((nerdDir() + "/music.ini").c_str(), "rb");
	if (!f)
		return;
	char buf[256];
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (const char *p = strstr(buf, "VOLUME="))
		mplay::setVolume(atoi(p + 7));
	if (const char *p = strstr(buf, "SHUFFLE="))
		shuffle = atoi(p + 8) != 0;
	if (const char *p = strstr(buf, "REPEAT=")) {
		const int r = atoi(p + 7);
		repeat = r == 0 ? Repeat::Off : (r == 2 ? Repeat::One : Repeat::All);
	}
}

void savePrefs() {
	mkdir((root() + "/_nds").c_str(), 0777);
	mkdir(nerdDir().c_str(), 0777);
	FILE *f = fopen((nerdDir() + "/music.ini").c_str(), "wb");
	if (!f)
		return;
	fprintf(f, "[MUSIC]\nVOLUME=%d\nSHUFFLE=%d\nREPEAT=%d\n", mplay::volume(), shuffle ? 1 : 0, (int)repeat);
	fclose(f);
}

void setNote(const char *s) {
	snprintf(note, sizeof(note), "%s", s);
	noteFrames = 150;
	bottomDirty = true;
}

// ---- top screen --------------------------------------------------------------------------------------------
inline u16 col(int r, int g, int b) { return (u16)(RGB15(r, g, b) | BIT(15)); }

u16 bgAt(int y) { return col(8 + y * 16 / 192, 24 + y * 7 / 192, 31); } // aqua to pale blue, Frutiger Aero style

void fillRect(u16 *buf, int x, int y, int w, int h, u16 c) {
	for (int j = y; j < y + h; j++) {
		if (j < 0 || j >= 192)
			continue;
		for (int i = x; i < x + w; i++)
			if (i >= 0 && i < 256)
				buf[j * 256 + i] = c;
	}
}

void fillCircle(u16 *buf, int cx, int cy, int r, u16 c) {
	for (int y = -r; y <= r; y++)
		for (int x = -r; x <= r; x++)
			if (x * x + y * y <= r * r) {
				const int px = cx + x, py = cy + y;
				if (px >= 0 && px < 256 && py >= 0 && py < 192)
					buf[py * 256 + px] = c;
			}
}

void fit(const char *s, int maxChars, char *out, size_t cap) {
	const int n = (int)strlen(s);
	if (n <= maxChars) {
		snprintf(out, cap, "%s", s);
		return;
	}
	snprintf(out, cap, "%.*s..", maxChars > 2 ? maxChars - 2 : 0, s);
}

uint32_t hashOf(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h;
}

// Generated cover art: a glossy disc whose colour comes from the album (or title), with a music note.
void drawDisc(u16 *buf, int cx, int cy, int r, uint32_t h) {
	static const int PAL[6][3] = {{31, 12, 18}, {10, 22, 31}, {12, 28, 14}, {31, 24, 8}, {22, 13, 30}, {6, 26, 25}};
	const int *b = PAL[h % 6];
	for (int y = -r; y <= r; y++)
		for (int x = -r; x <= r; x++) {
			const int d2 = x * x + y * y;
			if (d2 > r * r)
				continue;
			int rr, gg, bb;
			if (d2 > (r - 2) * (r - 2)) {
				rr = gg = bb = 31;
			} else {
				const int k = 20 + 11 * (r * r - d2) / (r * r); // 20..31 of 31
				rr = b[0] * k / 31;
				gg = b[1] * k / 31;
				bb = b[2] * k / 31;
				const int gy = y + r * 11 / 20;
				if (y < 0 && x * x * 100 / (r * r * 60) + gy * gy * 100 / (r * r * 12) < 100) { // gloss
					rr = (rr + 31) / 2;
					gg = (gg + 31) / 2;
					bb = (bb + 31) / 2;
				}
			}
			buf[(cy + y) * 256 + cx + x] = col(rr, gg, bb);
		}
	const u16 w = col(31, 31, 31);
	fillCircle(buf, cx - 9, cy + 11, 6, w);
	fillCircle(buf, cx + 9, cy + 7, 6, w);
	fillRect(buf, cx - 5, cy - 14, 3, 25, w);
	fillRect(buf, cx + 13, cy - 18, 3, 25, w);
	for (int i = 0; i < 4; i++)
		fillRect(buf, cx - 5, cy - 16 + i, 21, 1, w); // beam
}

void drawVu(u16 *buf, int l, int r) {
	const int x0 = 28, w = 200;
	for (int row = 0; row < 2; row++) {
		const int y = 176 + row * 6;
		for (int j = 0; j < 4; j++)
			fillRect(buf, x0, y + j, w, 1, bgAt(y + j));
		const int v = row == 0 ? l : r;
		int len = (int)((uint64_t)v * w / 24000);
		if (len > w)
			len = w;
		fillRect(buf, x0, y, len, 4, len > w * 85 / 100 ? col(31, 14, 8) : col(3, 20, 31));
	}
}

void drawTop() {
	const int next = topPage ^ 1;
	u16 *buf = uiTopPage(next);
	for (int y = 0; y < 192; y++)
		dmaFillHalfWords(bgAt(y), buf + y * 256, 512);

	const u16 ink = col(1, 5, 12), soft = col(4, 10, 18);
	char line[64], tmp[64];
	const mplay::State s = mplay::state();
	if (cur >= 0 && cur < trackCount && s != mplay::State::Idle) {
		const nmmusic::Track &t = tracks[cur];
		fit(t.title, 20, tmp, sizeof(tmp));
		nmfont::drawCentred(buf, 256, 192, 10, tmp, 2, ink);
		fit(t.artist[0] ? t.artist : "Unknown artist", 40, tmp, sizeof(tmp));
		nmfont::drawCentred(buf, 256, 192, 32, tmp, 1, soft);
		fit(t.album[0] ? t.album : "", 40, tmp, sizeof(tmp));
		nmfont::drawCentred(buf, 256, 192, 44, tmp, 1, soft);
		drawDisc(buf, 128, 94, 34, hashOf(t.album[0] ? t.album : t.title));
	} else {
		nmfont::drawCentred(buf, 256, 192, 10, "Music", 2, ink);
		nmfont::drawCentred(buf, 256, 192, 32, trackCount ? "Choose a track" : "No music found", 1, soft);
		drawDisc(buf, 128, 94, 34, 3);
	}

	// progress
	const uint32_t pos = mplay::positionMs(), dur = mplay::durationMs();
	fillRect(buf, 28, 142, 200, 8, col(31, 31, 31));
	fillRect(buf, 29, 143, 198, 6, col(20, 26, 31));
	if (dur) {
		int len = (int)((uint64_t)pos * 198 / dur);
		if (len > 198)
			len = 198;
		fillRect(buf, 29, 143, len, 6, col(3, 18, 31));
		fillRect(buf, 29, 143, len, 2, col(14, 26, 31));
	}
	nmmusic::formatTime(s == mplay::State::Idle ? 0 : pos, line, sizeof(line));
	nmfont::draw(buf, 256, 192, 28, 154, line, 1, ink);
	nmmusic::formatTime(dur ? dur : 0xFFFFFFFFu, line, sizeof(line));
	nmfont::draw(buf, 256, 192, 228 - nmfont::textWidth(line, 1), 154, line, 1, ink);

	const uint32_t hz = mplay::sampleRate();
	if (s == mplay::State::Error) {
		snprintf(line, sizeof(line), "%s", mplay::lastError());
	} else if (s == mplay::State::Idle || !hz) {
		snprintf(line, sizeof(line), "%s", mplay::stateName());
	} else if (mplay::underruns()) {
		snprintf(line, sizeof(line), "%s  %lu.%lu kHz  underruns %lu", mplay::stateName(), (unsigned long)(hz / 1000), (unsigned long)((hz % 1000) / 100), (unsigned long)mplay::underruns());
	} else {
		snprintf(line, sizeof(line), "%s  %lu.%lu kHz", mplay::stateName(), (unsigned long)(hz / 1000), (unsigned long)((hz % 1000) / 100));
	}
	nmfont::drawCentred(buf, 256, 192, 166, line, 1, soft);
	drawVu(buf, vuL, vuR);

	uiTopShowPage(next);
	topPage = next;
	topDirty = false;
}

void updateVu() {
	int l, r;
	mplay::levels(l, r);
	const bool playing = mplay::state() == mplay::State::Playing;
	vuL = playing ? (l > vuL ? l : vuL * 7 / 8) : vuL * 3 / 4;
	vuR = playing ? (r > vuR ? r : vuR * 7 / 8) : vuR * 3 / 4;
	drawVu(uiTopPage(topPage), vuL, vuR);
}

// ---- bottom screen -----------------------------------------------------------------------------------------
void drawPlayerBottom() {
	uiTextClear();
	uiBottomDrawBackground();
	const bool playing = mplay::state() == mplay::State::Playing;
	const UiButton b[] = {
		{B_PREV, 4, 70, 56, 62, UI_BTN_PREV, UI_BTN_PREV_P, true},
		{B_PLAY, 68, 70, 56, 62, playing ? UI_BTN_PAUSE : UI_BTN_PLAY, playing ? UI_BTN_PAUSE_P : UI_BTN_PLAY_P, true},
		{B_NEXT, 132, 70, 56, 62, UI_BTN_NEXT, UI_BTN_NEXT_P, true},
		{B_LIB, 196, 70, 56, 62, UI_BTN_ALBUM, UI_BTN_ALBUM_P, true},
		{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true},
	};
	uiShowButtons(b, 5);
	char line[40];
	snprintf(line, sizeof(line), "X Shuffle:%-3s  Y Repeat:%s", shuffle ? "On" : "Off", repeat == Repeat::Off ? "Off" : (repeat == Repeat::All ? "All" : "One"));
	uiTextAt(1, 4, line);
	snprintf(line, sizeof(line), "[-]   Volume %3d%%   [+]", mplay::volume() * 100 / 127);
	uiTextAt(3, 5, line);
	char bar[40];
	const uint32_t dur = mplay::durationMs();
	musicmath::bar(dur ? (uint32_t)((uint64_t)mplay::positionMs() * 1000 / dur) : 0, 30, bar);
	uiTextAt(1, 7, bar);
	char bt[24];
	snprintf(bt, sizeof(bt), "Music %d/%d", cur >= 0 ? cur + 1 : 0, trackCount);
	uiBarText(bt);
	uiStatus(noteFrames > 0 ? note : mplay::stateName());
	bottomDirty = false;
}

void refreshBar() { // the text progress bar only
	char bar[40];
	const uint32_t dur = mplay::durationMs();
	musicmath::bar(dur ? (uint32_t)((uint64_t)mplay::positionMs() * 1000 / dur) : 0, 30, bar);
	uiTextAt(1, 7, bar);
}

constexpr int ROWS = 12;

void drawLibraryBottom() {
	uiTextClear();
	uiBottomDrawBackground();
	const UiButton back[] = {{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true}};
	uiShowButtons(back, 1);
	char line[64];
	if (trackCount == 0) {
		uiTextCentred(6, "No music found.");
		uiTextCentred(8, "Put MP3 or WAV files in");
		uiTextCentred(9, "sd:/Music");
		uiTextCentred(11, "Y: scan again");
		uiBarText("0 tracks");
		uiStatus("Library empty");
		bottomDirty = false;
		return;
	}
	if (sel < scroll)
		scroll = sel;
	if (sel >= scroll + ROWS)
		scroll = sel - ROWS + 1;
	for (int r = 0; r < ROWS && scroll + r < trackCount; r++) {
		const nmmusic::Track &t = tracks[scroll + r];
		char name[64], tm[12];
		snprintf(name, sizeof(name), "%s%s%s", t.artist, t.artist[0] ? " - " : "", t.title);
		nmmusic::formatTime(t.durationMs ? t.durationMs : 0xFFFFFFFFu, tm, sizeof(tm));
		snprintf(line, sizeof(line), "%c%-22.22s %5s", (scroll + r == cur) ? '*' : (scroll + r == sel ? '>' : ' '), name, tm);
		uiTextAt(1, 3 + r, line);
	}
	uiTextCentred(16, "A: play  Y: rescan  B: back");
	snprintf(line, sizeof(line), "%d / %d", sel + 1, trackCount);
	uiBarText(line);
	uiStatus(noteFrames > 0 ? note : "Library");
	bottomDirty = false;
}

void redrawBottom() {
	if (mode == Mode::Player)
		drawPlayerBottom();
	else
		drawLibraryBottom();
}

// ---- library -----------------------------------------------------------------------------------------------
void scanProgress(int found) {
	char line[32];
	snprintf(line, sizeof(line), "%d tracks found", found);
	uiTextAt(8, 10, line);
	swiWaitForVBlank();
}

void scanLibrary() {
	uiTextClear();
	uiBottomDrawBackground();
	uiTextCentred(8, "Scanning music...");
	const musiclib::ScanResult r = musiclib::scan(tracks, scanProgress);
	trackCount = r.count;
	if (cur >= trackCount)
		cur = -1;
	if (sel >= trackCount)
		sel = trackCount ? trackCount - 1 : 0;
	char s[40];
	if (r.truncated)
		snprintf(s, sizeof(s), "%d tracks (list full)", trackCount);
	else
		snprintf(s, sizeof(s), "%d tracks +%d -%d", trackCount, r.added, r.removed);
	setNote(s);
	bottomDirty = topDirty = true;
}

// ---- playback control --------------------------------------------------------------------------------------
bool playIndex(int i) {
	for (int attempt = 0; attempt < 3 && trackCount > 0; attempt++) {
		cur = i;
		topDirty = bottomDirty = true;
		if (mplay::open(tracks[cur])) {
			sel = cur;
			return true;
		}
		setNote(mplay::lastError());
		i = nmmusic::nextIndex(i, trackCount, false, rng, 1);
	}
	return false;
}

void step(int dir) {
	if (trackCount == 0)
		return;
	const int from = cur >= 0 ? cur : (dir > 0 ? trackCount - 1 : 0);
	playIndex(nmmusic::nextIndex(from, trackCount, shuffle, rng, dir));
	playedInRun++;
}

void onFinished() {
	if (repeat == Repeat::One && cur >= 0) {
		playIndex(cur);
		return;
	}
	const bool last = !shuffle ? (cur == trackCount - 1) : (playedInRun + 1 >= trackCount);
	if (repeat == Repeat::Off && last) {
		mplay::stop();
		topDirty = bottomDirty = true;
		return;
	}
	step(1);
}

void restartOrPrevious() {
	if (cur >= 0 && mplay::positionMs() > 3000 && mplay::state() != mplay::State::Idle) {
		playIndex(cur);
		return;
	}
	step(-1);
}

void toggleRepeat() {
	repeat = repeat == Repeat::Off ? Repeat::All : (repeat == Repeat::All ? Repeat::One : Repeat::Off);
	bottomDirty = true;
}

void togglePlay() {
	const mplay::State s = mplay::state();
	if (s == mplay::State::Playing || s == mplay::State::Paused || s == mplay::State::Buffering) {
		mplay::togglePause();
	} else if (trackCount > 0) {
		playIndex(cur >= 0 ? cur : (shuffle ? nmmusic::nextIndex(-1, trackCount, true, rng) : 0));
		playedInRun = 0;
	}
	bottomDirty = topDirty = true;
}

void changeVolume(int d) {
	mplay::setVolume(musicmath::stepVolume(mplay::volume(), d));
	bottomDirty = topDirty = true;
}

// ---- main loop ---------------------------------------------------------------------------------------------
void run() {
	mplay::State lastState = mplay::State::Idle;
	while (true) {
		scanKeys();
		const u32 down = keysDown(), up = keysUp(), rep = keysDownRepeat();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (powerExit())
			return;
		if (down & KEY_LID) {
			mplay::pause();
			lidSleep();
			bottomDirty = topDirty = true;
			continue;
		}

		mplay::update();
		const mplay::State st = mplay::state();
		if (st == mplay::State::Finished)
			onFinished();
		if (st != lastState) {
			lastState = st;
			bottomDirty = topDirty = true;
		}
		if (noteFrames > 0 && --noteFrames == 0)
			bottomDirty = true;

		if (mode == Mode::Player) {
			if ((down & KEY_B) || touched == B_BACK)
				return;
			if ((down & KEY_A) || touched == B_PLAY)
				togglePlay();
			else if ((down & KEY_R) || touched == B_NEXT)
				step(1);
			else if ((down & KEY_L) || touched == B_PREV)
				restartOrPrevious();
			else if ((down & KEY_SELECT) || touched == B_LIB) {
				mode = Mode::Library;
				bottomDirty = true;
			} else if (rep & KEY_RIGHT) {
				mplay::seekRelativeMs(10000);
				topDirty = true;
			} else if (rep & KEY_LEFT) {
				mplay::seekRelativeMs(-10000);
				topDirty = true;
			} else if (rep & KEY_UP)
				changeVolume(1);
			else if (rep & KEY_DOWN)
				changeVolume(-1);
			else if (down & KEY_X) {
				shuffle = !shuffle;
				playedInRun = 0;
				bottomDirty = true;
			} else if (down & KEY_Y)
				toggleRepeat();
			else if (down & KEY_TOUCH) {
				touchPosition t;
				touchRead(&t);
				if (t.py >= 4 * 8 && t.py < 5 * 8) {
					if (t.px < 128) {
						shuffle = !shuffle;
						playedInRun = 0;
						bottomDirty = true;
					} else
						toggleRepeat();
				} else if (t.py >= 5 * 8 && t.py < 6 * 8) {
					changeVolume(t.px < 128 ? -1 : 1);
				} else if (t.py >= 7 * 8 && t.py < 8 * 8 && mplay::durationMs()) {
					mplay::seekPermille(musicmath::touchPermille(t.px, 8, 240));
					topDirty = true;
				}
			}
		} else {
			if ((down & KEY_B) || touched == B_BACK) {
				if (cur >= 0 && mplay::state() != mplay::State::Idle) {
					mode = Mode::Player;
					bottomDirty = true;
				} else
					return;
			} else if (down & KEY_Y) {
				scanLibrary();
			} else if (trackCount > 0) {
				auto move = [&](int delta) {
					int v = sel + delta;
					if (v < 0)
						v = (delta < -1) ? 0 : trackCount - 1;
					else if (v >= trackCount)
						v = (delta > 1) ? trackCount - 1 : 0;
					sel = v;
					bottomDirty = true;
				};
				if (rep & KEY_UP)
					move(-1);
				else if (rep & KEY_DOWN)
					move(1);
				else if (down & KEY_L)
					move(-ROWS);
				else if (down & KEY_R)
					move(ROWS);
				else if (down & KEY_A) {
					playedInRun = 0;
					if (playIndex(sel))
						mode = Mode::Player;
					bottomDirty = true;
				} else if (down & KEY_TOUCH) {
					touchPosition t;
					touchRead(&t);
					if (t.py >= 3 * 8 && t.py < (3 + ROWS) * 8) {
						const int hit = scroll + (t.py / 8 - 3);
						if (hit < trackCount) {
							if (hit == sel) {
								playedInRun = 0;
								if (playIndex(sel))
									mode = Mode::Player;
							}
							sel = hit;
							bottomDirty = true;
						}
					}
				}
			}
		}

		if (bottomDirty)
			redrawBottom();
		const uint32_t sec = mplay::positionMs() / 1000;
		if (topDirty || sec != lastSecond) {
			lastSecond = sec;
			drawTop();
			if (mode == Mode::Player && !bottomDirty)
				refreshBar();
		}
		updateVu();
		swiWaitForVBlank();
	}
}

} // namespace

int main(int argc, char **argv) {
	fifoSendValue32(FIFO_PM, PM_REQ_SLEEP_DISABLE); // the lid is handled here
	defaultExceptionHandler();

	sys().initFilesystem(argc == 0 ? "sd:/_nds/TWiLightMenu/main.srldr" : argv[0]);
	sys().initArm7RegStatuses();
	useTwlCfg = (dsiFeatures() && (*(u8 *)0x02000400 != 0) && (*(u8 *)0x02000401 == 0) && (*(u8 *)0x02000402 == 0) && (*(u8 *)0x02000404 == 0) && (*(u8 *)0x02000448 != 0));

	uiInit();
	if (sys().fatInitOk())
		ms().loadSettings();
	keysSetRepeat(25, 5);

	if (!sys().fatInitOk()) {
		uiTextClear();
		uiTextCentred(8, "No SD card found.");
		for (int i = 0; i < 120; i++)
			swiWaitForVBlank();
		returnToMenu();
	}

	msclock::ensureStarted();
	loadPrefs();
	uiTopViewerInit();
	topPage = uiTopShownPage();
	uiTopOverlayVisible(false);
	scanLibrary();
	drawTop();
	uiTopFade(false, 4);
	mode = Mode::Library;
	run();
	savePrefs();
	returnToMenu();
	return 0;
}
