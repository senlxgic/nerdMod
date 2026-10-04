#include "photos.h"

#include "camera.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/systemdetails.h"

namespace {

constexpr int IMG_W = CAM_CAPTURE_WIDTH;  // 640
constexpr int IMG_H = CAM_CAPTURE_HEIGHT; // 480
constexpr int ROW_BYTES = IMG_W * 3;      // 1920, already a multiple of 4
constexpr u32 BMP_HEADER_BYTES = 54;
constexpr u32 BMP_FILE_BYTES = BMP_HEADER_BYTES + ROW_BYTES * IMG_H;

std::string deviceRoot() { return sys().isRunFromSD() ? "sd:" : "fat:"; }
std::string photoDir() { return deviceRoot() + "/_nds/nerdMod/photos"; }

inline int clampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Integer YCbCr -> RGB, same coefficients as the reference implementation.
inline void yuvToRgb(int y, int cb, int cr, u8 &r, u8 &g, u8 &b) {
	r = clampByte(y + cr + (cr >> 2) + (cr >> 3) + (cr >> 5));
	g = clampByte(y - ((cb >> 2) + (cb >> 4) + (cb >> 5)) - ((cr >> 1) + (cr >> 3) + (cr >> 4) + (cr >> 5)));
	b = clampByte(y + cb + (cb >> 1) + (cb >> 2) + (cb >> 6));
}

inline u16 rgb555(u8 r, u8 g, u8 b) { return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000); }

void putU16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void putU32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
u32 getU32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
u16 getU16(const u8 *p) { return p[0] | (p[1] << 8); }

bool fileExists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool isPhotoName(const char *name) {
	const size_t len = strlen(name);
	return len > 7 && strncasecmp(name, "NM_", 3) == 0 && strcasecmp(name + len - 4, ".bmp") == 0;
}

} // namespace

bool photosEnsureFolder(void) {
	const std::string root = deviceRoot();
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir((root + "/_nds/nerdMod").c_str(), 0777);
	mkdir(photoDir().c_str(), 0777);

	struct stat st;
	return stat(photoDir().c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

PhotoResult photosSaveYuv(const u16 *yuv422, std::string &outName) {
	if (!sys().fatInitOk() || !photosEnsureFolder())
		return PHOTO_NO_STORAGE;

	// Pick a name that does not exist yet
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	char base[40];
	if (t) {
		snprintf(base, sizeof(base), "NM_%04d%02d%02d_%02d%02d%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	} else {
		snprintf(base, sizeof(base), "NM_00000000_%06lu", (unsigned long)(now & 0xFFFFFF));
	}

	std::string name = std::string(base) + ".bmp";
	for (int n = 1; fileExists(photoDir() + "/" + name); n++) {
		if (n > 999)
			return PHOTO_WRITE_FAILED;
		char suffix[12];
		snprintf(suffix, sizeof(suffix), "_%d", n);
		name = std::string(base) + suffix + ".bmp";
	}
	const std::string path = photoDir() + "/" + name;

	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return PHOTO_WRITE_FAILED;

	// BITMAPFILEHEADER + BITMAPINFOHEADER (24 bpp, bottom-up, uncompressed)
	u8 header[BMP_HEADER_BYTES] = {0};
	header[0] = 'B';
	header[1] = 'M';
	putU32(header + 2, BMP_FILE_BYTES);
	putU32(header + 10, BMP_HEADER_BYTES);
	putU32(header + 14, 40);
	putU32(header + 18, IMG_W);
	putU32(header + 22, IMG_H);
	putU16(header + 26, 1);
	putU16(header + 28, 24);
	putU32(header + 34, ROW_BYTES * IMG_H);
	putU32(header + 38, 2835); // 72 dpi
	putU32(header + 42, 2835);

	// (No free-space pre-check on purpose: statvfs() has to scan the whole FAT on a large
	// card, which would stall every shot. A full card shows up as ENOSPC from the write.)
	errno = 0;
	bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);

	static u8 row[ROW_BYTES];
	// Bottom-up: the last camera row comes first
	for (int y = IMG_H - 1; y >= 0 && ok; y--) {
		const u8 *src = (const u8 *)(yuv422 + y * IMG_W);
		for (int x = 0; x < IMG_W; x += 2) {
			const int y1 = src[x * 2 + 0];
			const int cb = src[x * 2 + 1] - 0x80;
			const int y2 = src[x * 2 + 2];
			const int cr = src[x * 2 + 3] - 0x80;
			u8 r, g, b;
			yuvToRgb(y1, cb, cr, r, g, b);
			row[x * 3 + 0] = b; // BMP stores B, G, R
			row[x * 3 + 1] = g;
			row[x * 3 + 2] = r;
			yuvToRgb(y2, cb, cr, r, g, b);
			row[x * 3 + 3] = b;
			row[x * 3 + 4] = g;
			row[x * 3 + 5] = r;
		}
		ok = fwrite(row, 1, ROW_BYTES, f) == (size_t)ROW_BYTES;
	}

	int writeErrno = errno; // from the failing write, before fclose() can overwrite it
	if (fclose(f) != 0) {
		ok = false;
		if (writeErrno == 0)
			writeErrno = errno;
	}

	if (!ok) {
		remove(path.c_str()); // never leave a truncated photo behind
		return (writeErrno == ENOSPC) ? PHOTO_NO_SPACE : PHOTO_WRITE_FAILED;
	}

	outName = name;
	return PHOTO_OK;
}

void photosList(std::vector<std::string> &names, size_t maxCount) {
	names.clear();
	DIR *dir = opendir(photoDir().c_str());
	if (!dir)
		return;

	while (dirent *ent = readdir(dir)) {
		if (isPhotoName(ent->d_name))
			names.emplace_back(ent->d_name);
	}
	closedir(dir);

	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
	if (names.size() > maxCount)
		names.erase(names.begin(), names.end() - maxCount); // keep the newest
}

bool photosDrawScaled(const std::string &name, u16 *dst) {
	FILE *f = fopen((photoDir() + "/" + name).c_str(), "rb");
	if (!f)
		return false;

	u8 header[BMP_HEADER_BYTES];
	bool ok = fread(header, 1, sizeof(header), f) == sizeof(header) && header[0] == 'B' && header[1] == 'M' && getU32(header + 10) == BMP_HEADER_BYTES &&
			  getU32(header + 14) == 40 && getU32(header + 18) == (u32)IMG_W && getU32(header + 22) == (u32)IMG_H && getU16(header + 26) == 1 &&
			  getU16(header + 28) == 24 && getU32(header + 30) == 0 && getU32(header + 2) >= BMP_FILE_BYTES;

	static u8 row[ROW_BYTES];
	for (int dy = 0; dy < 192 && ok; dy++) {
		const int sy = (dy * 5) / 2;                 // 480 -> 192 (2.5x)
		const int fileRow = IMG_H - 1 - sy;          // BMP is bottom-up
		ok = fseek(f, BMP_HEADER_BYTES + (long)fileRow * ROW_BYTES, SEEK_SET) == 0 && fread(row, 1, ROW_BYTES, f) == (size_t)ROW_BYTES;
		if (!ok)
			break;
		for (int dx = 0; dx < 256; dx++) {
			const int sx = (dx * 5) / 2; // 640 -> 256
			dst[dy * 256 + dx] = rgb555(row[sx * 3 + 2], row[sx * 3 + 1], row[sx * 3 + 0]);
		}
	}

	fclose(f);
	return ok;
}

void photosDrawYuvScaled(const u16 *yuv422, u16 *dst) {
	for (int dy = 0; dy < 192; dy++) {
		const int sy = (dy * 5) / 2;
		const u8 *src = (const u8 *)(yuv422 + sy * IMG_W);
		for (int dx = 0; dx < 256; dx++) {
			const int sx = (dx * 5) / 2;
			const int pair = sx & ~1;
			const int y = src[sx * 2];
			const int cb = src[pair * 2 + 1] - 0x80;
			const int cr = src[pair * 2 + 3] - 0x80;
			u8 r, g, b;
			yuvToRgb(y, cb, cr, r, g, b);
			dst[dy * 256 + dx] = rgb555(r, g, b);
		}
	}
}

bool photosDelete(const std::string &name) {
	return remove((photoDir() + "/" + name).c_str()) == 0;
}
