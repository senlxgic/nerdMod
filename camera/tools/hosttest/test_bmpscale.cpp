#include <stdio.h>
#include <string.h>
#include <vector>
#include "../../arm9/source/bmpscale.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace bmpscale;

static std::vector<uint8_t> makeBmp(int w, int hSigned, int bpp, uint32_t compression = 0, bool trunc = false) {
	int h = hSigned < 0 ? -hSigned : hSigned;
	uint32_t rb = ((uint32_t)w * (bpp / 8) + 3) & ~3u;
	uint32_t off = 54 + (compression == 3 ? 12 : 0);
	uint32_t size = off + rb * h - (trunc ? 100 : 0);
	std::vector<uint8_t> b(size, 0);
	b[0] = 'B'; b[1] = 'M';
	auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (v >> (8 * i)) & 255; };
	p32(2, size); p32(10, off); p32(14, 40); p32(18, w); p32(22, (uint32_t)hSigned);
	b[26] = 1; b[28] = bpp; p32(30, compression);
	if (compression == 3) { p32(54, 0x00FF0000); p32(58, 0x0000FF00); p32(62, 0xFF); }
	return b;
}

int main() {
	auto b = makeBmp(640, 480, 24);
	Info i = parse(b.data(), b.size(), b.size());
	CHECK(i.ok && i.w == 640 && i.h == 480 && i.bpp == 24 && !i.topDown && i.rowBytes == 1920);
	CHECK(rowOffset(i, 0) == 54 + 479ull * 1920);
	b = makeBmp(100, 50, 32);
	i = parse(b.data(), b.size(), b.size());
	CHECK(i.ok && i.bpp == 32 && i.rowBytes == 400);
	b = makeBmp(101, -50, 24);
	i = parse(b.data(), b.size(), b.size());
	CHECK(i.ok && i.topDown && i.rowBytes == 304 && rowOffset(i, 0) == 54 && rowOffset(i, 49) == 54 + 49ull * 304);
	b = makeBmp(64, 64, 32, 3);
	i = parse(b.data(), b.size(), b.size());
	CHECK(i.ok && i.dataOffset == 66);
	// rejects
	b = makeBmp(64, 64, 24, 1); CHECK(!parse(b.data(), b.size(), b.size()).ok);       // RLE
	b = makeBmp(64, 64, 8);     CHECK(!parse(b.data(), b.size(), b.size()).ok);       // palette
	b = makeBmp(64, 64, 24, 0, true); CHECK(!parse(b.data(), b.size(), b.size()).ok); // truncated
	b = makeBmp(64, 64, 24); b[0] = 'X'; CHECK(!parse(b.data(), b.size(), b.size()).ok);
	b = makeBmp(64, 64, 24); CHECK(!parse(b.data(), 20, b.size()).ok);
	b = makeBmp(0, 64, 24); CHECK(!parse(b.data(), b.size(), b.size()).ok);
	b = makeBmp(64, 64, 24); b[22] = 0; b[23] = 0; b[24] = 0; b[25] = 0; CHECK(!parse(b.data(), b.size(), b.size()).ok);
	{ std::vector<uint8_t> big(54, 0); big[0]='B'; big[1]='M'; big[14]=40; big[18]=0xFF; big[19]=0xFF; big[22]=0xFF; big[23]=0xFF; big[26]=1; big[28]=24; big[10]=54;
	  CHECK(!parse(big.data(), big.size(), 1 << 30).ok); }
	// fit: aspect kept, never outside the box, never zero
	int dw, dh;
	fit(640, 480, 256, 192, dw, dh); CHECK(dw == 256 && dh == 192);
	fit(1000, 100, 256, 192, dw, dh); CHECK(dw == 256 && dh == 25);
	fit(100, 1000, 256, 192, dw, dh); CHECK(dh == 192 && dw == 19);
	fit(1, 5000, 256, 192, dw, dh);   CHECK(dw == 1 && dh == 192);
	fit(8192, 1, 256, 192, dw, dh);   CHECK(dw == 256 && dh == 1);
	fit(16, 16, 256, 192, dw, dh);    CHECK(dw == 192 && dh == 192); // upscales small images to the box
	fit(0, 10, 256, 192, dw, dh);     CHECK(dw == 0 && dh == 0);
	for (int sw : {1, 2, 3, 77, 640, 4000, 8192})
		for (int sh : {1, 2, 5, 480, 8192}) {
			fit(sw, sh, 208, 156, dw, dh);
			CHECK(dw >= 1 && dw <= 208 && dh >= 1 && dh <= 156);
			for (int d : {0, dw / 2, dw - 1}) { int s = srcCoord(d, dw, sw); CHECK(s >= 0 && s < sw); }
			for (int d : {0, dh / 2, dh - 1}) { int s = srcCoord(d, dh, sh); CHECK(s >= 0 && s < sh); }
		}
	CHECK(srcCoord(0, 256, 640) == 1 && srcCoord(255, 256, 640) == 638);
	CHECK(rgb555(255, 255, 255) == 0xFFFF && rgb555(0, 0, 0) == 0x8000 && rgb555(255, 0, 0) == 0x801F);
	printf(failures ? "%d FAILURES\n" : "bmp tests OK\n", failures);
	return failures ? 1 : 0;
}
