/*
	nerdMod Photos

	A small photo browser launched from the home screen like any other app. It shows the pictures nerdMod knows about in
	categories (Photos, Home photos, Screenshots, Drawings, Other - empty ones are hidden) and can set one of them as the
	home screen photo. It reuses the Camera app's user-interface layer (ui.cpp) and display setup, which are the parts of the
	Camera Album that have been verified on a DSi.

	Controls:
	  Categories: Up/Down, A open, B back to the menu
	  List:       Up/Down (L/R page), A view, Y options, X info, B back
	  Viewer:     Left/Right or L/R previous/next, Y options, X info, A/B back
	  Options:    Set as Home Photo, Use default home photo, Delete, Info
*/

#include <nds.h>

#include <dirent.h>
#include <errno.h>
#include <fat.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "bmpscale.h"
#include "ui.h"

#include "common/lodepng.h"
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

constexpr size_t MAX_ITEMS = 512;
constexpr u32 MAX_PNG_PIXELS = 3u << 20; // 3 MP decoded as RGBA = 12 MB
constexpr u32 MAX_PNG_FILE = 8u << 20;

enum { B_BACK = 1 };

bool powerExit() { return fifoCheckValue32(FIFO_USER_01); }

std::string root() { return sys().isRunFromSD() ? "sd:" : "fat:"; }
std::string nerdDir() { return root() + "/_nds/nerdMod"; }
std::string photoDirBase() { return nerdDir() + "/photos"; }

[[noreturn]] void stopForever() {
	while (true)
		swiWaitForVBlank();
}

[[noreturn]] void returnToMenu() {
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

// ---- files ----------------------------------------------------------------------------------------------
bool endsWith(const char *name, const char *ext) {
	const size_t n = strlen(name), e = strlen(ext);
	return n > e && strcasecmp(name + n - e, ext) == 0;
}
bool isImageName(const char *name) { return name[0] != '.' && (endsWith(name, ".bmp") || endsWith(name, ".png")); }

struct Item {
	std::string path, name;
};

struct Category {
	const char *title;
	std::vector<std::string> dirs;
	std::vector<Item> items;
};

// Old and new locations both count; nothing is ever moved.
std::vector<Category> makeCategories() {
	const std::string p = photoDirBase();
	std::vector<Category> c(5);
	c[0] = {"Photos", {p, p + "/camera"}, {}};
	c[1] = {"Home photos", {p + "/home", root() + "/_nds/TWiLightMenu/dsimenu/photos"}, {}};
	c[2] = {"Screenshots", {p + "/screenshots"}, {}};
	c[3] = {"Drawings", {p + "/drawings"}, {}};
	c[4] = {"Other", {p + "/other"}, {}};
	return c;
}

void scanCategory(Category &c) {
	c.items.clear();
	for (const std::string &dir : c.dirs) {
		DIR *d = opendir(dir.c_str());
		if (!d)
			continue;
		while (dirent *e = readdir(d)) {
			if (e->d_type == DT_DIR || !isImageName(e->d_name))
				continue;
			c.items.push_back({dir + "/" + e->d_name, e->d_name});
			if (c.items.size() > 4 * MAX_ITEMS)
				break;
		}
		closedir(d);
	}
	std::sort(c.items.begin(), c.items.end(), [](const Item &a, const Item &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) > 0; }); // newest first
	if (c.items.size() > MAX_ITEMS)
		c.items.resize(MAX_ITEMS);
}

// ---- decoding --------------------------------------------------------------------------------------------
struct ImageInfo {
	bool ok = false;
	int w = 0, h = 0;
	const char *format = "?";
	u32 bytes = 0;
	const char *error = "";
};

u32 be32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Draws the picture into a 256x192 RGB555 page: aspect kept, black bars.
ImageInfo drawImage(const std::string &path, u16 *dst) {
	ImageInfo info;
	dmaFillHalfWords(0x8000, dst, 256 * 192 * 2);
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		info.error = "Cannot open the file";
		return info;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	info.bytes = size > 0 ? (u32)size : 0;

	u8 head[96];
	const size_t got = fread(head, 1, sizeof(head), f);

	if (got >= 54 && head[0] == 'B' && head[1] == 'M') {
		info.format = "BMP";
		const bmpscale::Info b = bmpscale::parse(head, got, info.bytes);
		if (!b.ok) {
			info.error = "Unsupported BMP";
			fclose(f);
			return info;
		}
		info.w = b.w;
		info.h = b.h;
		int dw, dh;
		bmpscale::fit(b.w, b.h, 256, 192, dw, dh);
		u8 *row = (u8 *)malloc(b.rowBytes);
		if (!row) {
			info.error = "Out of memory";
			fclose(f);
			return info;
		}
		const int x0 = (256 - dw) / 2, y0 = (192 - dh) / 2;
		const int bytesPer = b.bpp / 8;
		bool ok = true;
		int lastSy = -1;
		for (int dy = 0; dy < dh && ok; dy++) {
			const int sy = bmpscale::srcCoord(dy, dh, b.h);
			if (sy != lastSy) {
				ok = fseek(f, (long)bmpscale::rowOffset(b, sy), SEEK_SET) == 0 && fread(row, 1, b.rowBytes, f) == b.rowBytes;
				lastSy = sy;
			}
			if (!ok)
				break;
			u16 *out = dst + (y0 + dy) * 256 + x0;
			for (int dx = 0; dx < dw; dx++) {
				const u8 *px = row + bmpscale::srcCoord(dx, dw, b.w) * bytesPer;
				out[dx] = bmpscale::rgb555(px[2], px[1], px[0]);
			}
		}
		free(row);
		fclose(f);
		if (!ok)
			info.error = "Read error";
		info.ok = ok;
		return info;
	}

	static const u8 pngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (got >= 24 && memcmp(head, pngSig, 8) == 0) {
		info.format = "PNG";
		fclose(f);
		const u32 w = be32(head + 16), h = be32(head + 20);
		info.w = (int)w;
		info.h = (int)h;
		if (w == 0 || h == 0 || w > 8192 || h > 8192 || (u64)w * h > MAX_PNG_PIXELS || info.bytes > MAX_PNG_FILE) {
			info.error = "Picture too large";
			return info;
		}
		std::vector<unsigned char> img;
		unsigned iw = 0, ih = 0;
		if (lodepng::decode(img, iw, ih, path) != 0 || iw != w || ih != h || img.size() < (size_t)iw * ih * 4) {
			info.error = "Cannot decode PNG";
			return info;
		}
		int dw, dh;
		bmpscale::fit((int)iw, (int)ih, 256, 192, dw, dh);
		const int x0 = (256 - dw) / 2, y0 = (192 - dh) / 2;
		for (int dy = 0; dy < dh; dy++) {
			const int sy = bmpscale::srcCoord(dy, dh, (int)ih);
			u16 *out = dst + (y0 + dy) * 256 + x0;
			for (int dx = 0; dx < dw; dx++) {
				const u8 *px = &img[((size_t)sy * iw + bmpscale::srcCoord(dx, dw, (int)iw)) * 4];
				const unsigned a = px[3]; // composite on black
				out[dx] = bmpscale::rgb555((u8)(px[0] * a / 255), (u8)(px[1] * a / 255), (u8)(px[2] * a / 255));
			}
		}
		info.ok = true;
		return info;
	}

	fclose(f);
	info.error = "Not a BMP or PNG";
	return info;
}

// ---- home photo manifest ---------------------------------------------------------------------------------
// sd:/_nds/nerdMod/home-photo.ini   [HOME] PATH=<file>  SERIAL=<n>
// The home screen reads PATH first (see romsel_dsimenutheme graphics.cpp loadPhotoList); SERIAL changes on every write so a
// cached copy can be recognised as stale. An empty PATH means "use the default folders".
int readSerial(const std::string &file) {
	FILE *f = fopen(file.c_str(), "rb");
	if (!f)
		return 0;
	char buf[512];
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	const char *p = strstr(buf, "SERIAL=");
	return p ? atoi(p + 7) : 0;
}

bool writeHomeManifest(const std::string &path, int &err) {
	err = 0;
	mkdir((root() + "/_nds").c_str(), 0777);
	mkdir(nerdDir().c_str(), 0777);
	const std::string file = nerdDir() + "/home-photo.ini";
	const int serial = readSerial(file) + 1;
	FILE *f = fopen(file.c_str(), "wb");
	if (!f) {
		err = errno;
		return false;
	}
	const int w = fprintf(f, "[HOME]\nPATH=%s\nSERIAL=%d\n", path.c_str(), serial);
	const int c = fclose(f);
	if (w < 0 || c != 0) {
		err = errno ? errno : EIO;
		return false;
	}
	return readSerial(file) == serial; // read it back
}

// ---- screens ---------------------------------------------------------------------------------------------
int topPage = 0;

void showOnTop(const std::string &path, ImageInfo *out = nullptr) {
	const int next = topPage ^ 1;
	const ImageInfo i = drawImage(path, uiTopPage(next));
	topPage = next;
	uiTopShowPage(topPage);
	if (out)
		*out = i;
}

void clearTopScreen() {
	const int next = topPage ^ 1;
	dmaFillHalfWords(0x8000, uiTopPage(next), 256 * 192 * 2);
	topPage = next;
	uiTopShowPage(topPage);
}

void shortName(char *out, size_t n, const std::string &s, size_t width) {
	snprintf(out, n, "%.*s", (int)width, s.c_str());
}

// Waits for A or B (or touch); returns false on power exit.
bool waitForKey() {
	while (true) {
		scanKeys();
		if (powerExit())
			return false;
		if (keysDown() & (KEY_A | KEY_B | KEY_TOUCH | KEY_X | KEY_Y))
			return true;
		swiWaitForVBlank();
	}
}

// Modal list: returns the chosen index, -1 for B, -2 for a power exit.
int pickFromList(const char *title, const std::vector<std::string> &entries) {
	int sel = 0;
	bool redraw = true;
	while (true) {
		if (redraw) {
			redraw = false;
			uiTextClear();
			uiBottomDrawBackground();
			uiDrawDialogPanel();
			uiTextCentred(6, title);
			for (size_t i = 0; i < entries.size() && i < 5; i++) {
				char line[40];
				snprintf(line, sizeof(line), "%c %.26s", (int)i == sel ? '>' : ' ', entries[i].c_str());
				uiTextAt(4, 8 + (int)i, line);
			}
		}
		scanKeys();
		const u32 down = keysDown(), rep = keysDownRepeat();
		if (powerExit())
			return -2;
		if (rep & KEY_UP) {
			sel = (sel + (int)entries.size() - 1) % (int)entries.size();
			redraw = true;
		} else if (rep & KEY_DOWN) {
			sel = (sel + 1) % (int)entries.size();
			redraw = true;
		} else if (down & KEY_A) {
			return sel;
		} else if (down & KEY_B) {
			return -1;
		}
		swiWaitForVBlank();
	}
}

bool showInfo(const Item &it, const ImageInfo &ii, const char *categoryTitle) {
	uiTextClear();
	uiBottomDrawBackground();
	uiDrawDialogPanel();
	uiTextCentred(6, "Photo info");
	char line[48];
	shortName(line, sizeof(line), it.name, 26);
	uiTextAt(4, 8, line);
	snprintf(line, sizeof(line), "%s  %dx%d", ii.format, ii.w, ii.h);
	uiTextAt(4, 9, line);
	snprintf(line, sizeof(line), "%lu KB", (unsigned long)((ii.bytes + 1023) / 1024));
	uiTextAt(4, 10, line);
	snprintf(line, sizeof(line), "In: %.24s", categoryTitle);
	uiTextAt(4, 11, line);
	if (!ii.ok) {
		snprintf(line, sizeof(line), "%.28s", ii.error);
		uiTextAt(4, 12, line);
	}
	return waitForKey();
}

void message(const char *a, const char *b = "") {
	uiTextClear();
	uiBottomDrawBackground();
	uiDrawDialogPanel();
	uiTextCentred(8, a);
	if (b[0])
		uiTextCentred(10, b);
	for (int i = 0; i < 100; i++) {
		scanKeys();
		if ((keysDown() & (KEY_A | KEY_B | KEY_TOUCH)) || powerExit())
			break;
		swiWaitForVBlank();
	}
}

enum class Result { Stay, Deleted, Power };

// Options for one picture. Returns Deleted when the file is gone.
Result optionsMenu(Category &cat, size_t index, const ImageInfo &ii) {
	const Item it = cat.items[index];
	const std::vector<std::string> entries = {"Set as Home Photo", "Use default home photo", "Delete", "Info", "Cancel"};
	const int c = pickFromList("Options", entries);
	if (c == -2)
		return Result::Power;
	if (c == 0) {
		int err = 0;
		if (writeHomeManifest(it.path, err))
			message("Home photo set.", "Shown after the menu restarts.");
		else {
			char e[32];
			snprintf(e, sizeof(e), "(error %d)", err);
			message("Could not save the setting.", e);
		}
	} else if (c == 1) {
		int err = 0;
		if (writeHomeManifest("", err))
			message("Home photo reset.", "The default photos are used.");
		else
			message("Could not save the setting.");
	} else if (c == 2) {
		const int d = pickFromList("Delete this photo?", {"Delete", "Cancel"});
		if (d == -2)
			return Result::Power;
		if (d == 0) {
			if (remove(it.path.c_str()) == 0) {
				cat.items.erase(cat.items.begin() + index);
				return Result::Deleted;
			}
			char e[40];
			snprintf(e, sizeof(e), "(error %d)", errno);
			message("Could not delete the file.", e);
		}
	} else if (c == 3) {
		if (!showInfo(it, ii, cat.title))
			return Result::Power;
	}
	return Result::Stay;
}

// Full-screen viewer. Returns false on power exit.
bool viewer(Category &cat, size_t &index) {
	bool redraw = true;
	ImageInfo ii;
	while (true) {
		if (cat.items.empty())
			return true;
		if (index >= cat.items.size())
			index = cat.items.size() - 1;
		if (redraw) {
			redraw = false;
			showOnTop(cat.items[index].path, &ii);
			uiTextClear();
			uiBottomDrawBackground();
			const UiButton back[] = {{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true}};
			uiShowButtons(back, 1);
			char line[40];
			snprintf(line, sizeof(line), "%d / %d", (int)index + 1, (int)cat.items.size());
			uiBarText(line);
			shortName(line, sizeof(line), cat.items[index].name, 30);
			uiTextCentred(4, line);
			if (!ii.ok)
				uiTextCentred(6, ii.error);
			else {
				snprintf(line, sizeof(line), "%s  %dx%d", ii.format, ii.w, ii.h);
				uiTextCentred(6, line);
			}
			uiTextCentred(8, "Y: options   X: info");
			uiStatus(cat.title);
		}
		scanKeys();
		const u32 down = keysDown(), up = keysUp();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (powerExit())
			return false;
		if (down & KEY_LID) {
			lidSleep();
			continue;
		}
		const size_t n = cat.items.size();
		if ((down & (KEY_B | KEY_A)) || touched == B_BACK)
			return true;
		if (down & (KEY_LEFT | KEY_L)) {
			index = (index + n - 1) % n;
			redraw = true;
		} else if (down & (KEY_RIGHT | KEY_R)) {
			index = (index + 1) % n;
			redraw = true;
		} else if (down & KEY_X) {
			if (!showInfo(cat.items[index], ii, cat.title))
				return false;
			redraw = true;
		} else if (down & KEY_Y) {
			const Result r = optionsMenu(cat, index, ii);
			if (r == Result::Power)
				return false;
			redraw = true;
		}
		swiWaitForVBlank();
	}
}

// Thumbnail list of one category. Returns false on power exit.
bool listScreen(Category &cat) {
	size_t sel = 0, scroll = 0;
	bool redraw = true;
	int previewTimer = 0;
	bool previewPending = true;
	constexpr int ROWS = 12;
	ImageInfo ii;
	while (true) {
		if (cat.items.empty()) {
			clearTopScreen();
			message("No pictures here.");
			return true;
		}
		if (sel >= cat.items.size())
			sel = cat.items.size() - 1;
		if (sel < scroll)
			scroll = sel;
		if (sel >= scroll + ROWS)
			scroll = sel - ROWS + 1;
		if (redraw) {
			redraw = false;
			uiTextClear();
			uiBottomDrawBackground();
			const UiButton back[] = {{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true}};
			uiShowButtons(back, 1);
			char line[48];
			snprintf(line, sizeof(line), "%d / %d", (int)sel + 1, (int)cat.items.size());
			uiBarText(line);
			for (int r = 0; r < ROWS && scroll + r < cat.items.size(); r++) {
				snprintf(line, sizeof(line), "%c%.29s", scroll + r == sel ? '>' : ' ', cat.items[scroll + r].name.c_str());
				uiTextAt(1, 3 + r, line);
			}
			uiTextCentred(16, "A: view   Y: options");
			uiStatus(cat.title);
		}
		if (previewPending && --previewTimer <= 0) {
			previewPending = false;
			showOnTop(cat.items[sel].path, &ii); // debounced: not on every scroll step
		}
		scanKeys();
		const u32 down = keysDown(), up = keysUp(), rep = keysDownRepeat();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (powerExit())
			return false;
		if (down & KEY_LID) {
			lidSleep();
			continue;
		}
		if ((down & KEY_B) || touched == B_BACK)
			return true;
		const size_t n = cat.items.size();
		auto move = [&](long delta) {
			long v = (long)sel + delta;
			if (v < 0)
				v = (delta < -1) ? 0 : (long)n - 1;
			else if (v >= (long)n)
				v = (delta > 1) ? (long)n - 1 : 0;
			sel = (size_t)v;
			redraw = true;
			previewPending = true;
			previewTimer = 8;
		};
		if (rep & KEY_UP)
			move(-1);
		else if (rep & KEY_DOWN)
			move(1);
		else if (down & KEY_L)
			move(-ROWS);
		else if (down & KEY_R)
			move(ROWS);
		else if (down & KEY_TOUCH) {
			touchPosition t;
			touchRead(&t);
			if (t.py >= 3 * 8 && t.py < (3 + ROWS) * 8) {
				const size_t hit = scroll + (t.py / 8 - 3);
				if (hit < n && hit == sel) {
					if (!viewer(cat, sel))
						return false;
					redraw = true;
					previewPending = true;
					previewTimer = 1;
				} else if (hit < n) {
					sel = hit;
					redraw = true;
					previewPending = true;
					previewTimer = 8;
				}
			}
		} else if (down & KEY_A) {
			if (!viewer(cat, sel))
				return false;
			redraw = true;
			previewPending = true;
			previewTimer = 1;
		} else if (down & KEY_X) {
			if (previewPending)
				showOnTop(cat.items[sel].path, &ii), previewPending = false;
			if (!showInfo(cat.items[sel], ii, cat.title))
				return false;
			redraw = true;
		} else if (down & KEY_Y) {
			if (previewPending)
				showOnTop(cat.items[sel].path, &ii), previewPending = false;
			const Result r = optionsMenu(cat, sel, ii);
			if (r == Result::Power)
				return false;
			redraw = true;
			if (r == Result::Deleted) {
				previewPending = true;
				previewTimer = 1;
			}
		}
		swiWaitForVBlank();
	}
}

void categoriesScreen() {
	std::vector<Category> cats = makeCategories();
	std::vector<int> visible;
	auto rescan = [&]() {
		visible.clear();
		for (size_t i = 0; i < cats.size(); i++) {
			scanCategory(cats[i]);
			if (!cats[i].items.empty())
				visible.push_back((int)i);
		}
	};
	rescan();
	int sel = 0;
	bool redraw = true;
	while (true) {
		if (redraw) {
			redraw = false;
			uiTextClear();
			uiBottomDrawBackground();
			const UiButton back[] = {{B_BACK, UI_RECT_BACK, UI_BTN_BACK, UI_BTN_BACK_P, true}};
			uiShowButtons(back, 1);
			uiBarText("Photos");
			if (visible.empty()) {
				uiTextCentred(7, "No photos yet.");
				uiTextCentred(9, "Take some with Camera!");
			}
			for (size_t i = 0; i < visible.size(); i++) {
				char line[48];
				snprintf(line, sizeof(line), "%c %-16s %3d", (int)i == sel ? '>' : ' ', cats[visible[i]].title, (int)cats[visible[i]].items.size());
				uiTextAt(4, 5 + (int)i * 2, line);
			}
			uiStatus("Photos");
		}
		scanKeys();
		const u32 down = keysDown(), up = keysUp(), rep = keysDownRepeat();
		const int touched = uiHandleInput(down, up);
		uiTick();
		if (powerExit())
			return;
		if (down & KEY_LID) {
			lidSleep();
			continue;
		}
		if ((down & KEY_B) || touched == B_BACK)
			return;
		if (!visible.empty()) {
			const int n = (int)visible.size();
			if (rep & KEY_UP) {
				sel = (sel + n - 1) % n;
				redraw = true;
			} else if (rep & KEY_DOWN) {
				sel = (sel + 1) % n;
				redraw = true;
			} else if (down & KEY_A) {
				const bool alive = listScreen(cats[visible[sel]]);
				if (!alive)
					return;
				rescan();
				if (sel >= (int)visible.size())
					sel = visible.empty() ? 0 : (int)visible.size() - 1;
				clearTopScreen();
				redraw = true;
			}
		}
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

	uiTopViewerInit();
	topPage = uiTopShownPage();
	uiTopOverlayVisible(false);
	uiTopFade(false, 4);
	categoriesScreen();
	returnToMenu();
	return 0;
}
