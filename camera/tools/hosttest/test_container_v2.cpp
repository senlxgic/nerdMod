// Host test of NERDVID v2 (RGB332 / RGB332 half-res) and of reading version 1 files. Also covers videofmt.h.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "../../arm9/source/videoContainer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static u16 pixel(int n, u32 x, u32 y) {
	const u32 r = (x * 31 / 255 + n) & 31, g = (y * 31 / 191 + n * 3) & 31, b = ((x + y) * 31 / 446 + n * 5) & 31;
	return (u16)(0x8000 | r | (g << 5) | (b << 10));
}

static void makeFrame(u16 *f, int n) {
	for (u32 y = 0; y < 192; y++)
		for (u32 x = 0; x < 256; x++) f[y * 256 + x] = pixel(n, x, y);
}

static int chan(u16 c, int s) { return (c >> s) & 31; }

int main() {
	// ---- format arithmetic
	CHECK(vfmt::frameBytes(vfmt::F_RGB555) == 98304);
	CHECK(vfmt::frameBytes(vfmt::F_RGB332) == 49152);
	CHECK(vfmt::frameBytes(vfmt::F_RGB332_HALF) == 12288);
	for (u16 f : {vfmt::F_RGB555, vfmt::F_RGB332, vfmt::F_RGB332_HALF}) {
		CHECK(vfmt::frameBytes(f) % 512 == 0);
		CHECK(nvid::slotBytesFor(f) % 512 == 0);
	}
	CHECK(nvid::slotBytesFor(vfmt::F_RGB555) == nvid::VIDEO_SLOT_BYTES);
	CHECK(vfmt::frameBytes(99) == 0);
	CHECK(vfmt::sanitizeQuality(7) == vfmt::DEFAULT_QUALITY);

	// ---- RGB332 round trip: error is at most the quantisation step; pure white/black stay put
	CHECK(vfmt::fromRgb332(vfmt::toRgb332(0xFFFF)) == 0xFFFF);
	CHECK(vfmt::fromRgb332(vfmt::toRgb332(0x8000)) == 0x8000);
	for (u32 c = 0; c < 32768; c += 7) {
		const u16 px = (u16)(0x8000 | c);
		const u16 back = vfmt::fromRgb332(vfmt::toRgb332(px));
		CHECK(abs(chan(px, 0) - chan(back, 0)) <= 4);
		CHECK(abs(chan(px, 5) - chan(back, 5)) <= 4);
		CHECK(abs(chan(px, 10) - chan(back, 10)) <= 8);
		CHECK(back & 0x8000);
	}

	u16 *src = (u16 *)aligned_alloc(32, 98304);
	u16 *out = (u16 *)aligned_alloc(32, 98304);
	u8 *slot = (u8 *)aligned_alloc(32, nvid::VIDEO_SLOT_BYTES);

	// ---- write + read each format through the real Writer/Reader
	const u16 formats[3] = {vfmt::F_RGB555, vfmt::F_RGB332, vfmt::F_RGB332_HALF};
	for (u16 fmt : formats) {
		const char *path = "/tmp/nvid_v2.nvid";
		nvid::Writer w;
		nvid::WriteParams p;
		p.audio = true;
		p.videoFormat = fmt;
		CHECK(w.open(path, p));
		u8 *audio = (u8 *)aligned_alloc(32, 32 + 4096 + nvid::AUDIO_BLOCK_EXTRA);
		for (int n = 0; n < 12; n++) {
			makeFrame(src, n);
			memset(slot, 0xEE, nvid::slotBytesFor(fmt));
			vfmt::encode(fmt, src, slot + nvid::VIDEO_FRAME_OFFSET);
			CHECK(w.writeVideoSlot(slot, n * 100));
			if (n % 4 == 3) {
				memset(audio + 16, n, 4096);
				CHECK(w.writeAudioBlock(audio, 4096, n * 100));
			}
		}
		nvid::Final fin;
		fin.durationMs = 1200;
		CHECK(w.finish(fin));
		CHECK(w.alignedWrites() == 12 + 3);
		free(audio);

		nvid::Header h;
		CHECK(nvid::readHeader(path, h));
		CHECK(h.version == 2 && h.videoFormat == fmt);
		CHECK(h.width == vfmt::widthOf(fmt) && h.height == vfmt::heightOf(fmt));

		nvid::Reader r;
		CHECK(r.open(path));
		CHECK(r.frameCount() == 12 && r.payloadBytes() == vfmt::frameBytes(fmt));
		// the expected picture is "encode then decode" of the source
		u16 *expect = (u16 *)aligned_alloc(32, 98304);
		u8 *tmp = (u8 *)aligned_alloc(32, 98304);
		for (int n : {0, 5, 11}) {
			makeFrame(src, n);
			vfmt::encode(fmt, src, tmp);
			vfmt::decode(fmt, tmp, expect);
			u32 t = 0;
			CHECK(r.readFrame((u32)n, out, &t) && t == (u32)n * 100);
			CHECK(memcmp(out, expect, 98304) == 0);
		}
		// sequential playback path
		CHECK(r.rewind());
		nvid::Reader::Chunk c;
		int v = 0, au = 0;
		while (r.nextChunk(c)) {
			if (c.fourcc == nvid::CHUNK_VIDEO) {
				CHECK(c.size == r.payloadBytes());
				CHECK(r.readVideoPayload(out));
				makeFrame(src, v);
				vfmt::encode(fmt, src, tmp);
				vfmt::decode(fmt, tmp, expect);
				CHECK(memcmp(out, expect, 98304) == 0);
				v++;
			} else {
				CHECK(c.fourcc == nvid::CHUNK_AUDIO);
				CHECK(r.skipPayload(c.size));
				au++;
			}
		}
		CHECK(v == 12 && au == 3);
		// file size matches the arithmetic: header block + slots + audio blocks + index
		struct stat_ { off_t s; } st_;
		int fd = open(path, O_RDONLY);
		st_.s = lseek(fd, 0, SEEK_END);
		close(fd);
		CHECK((u32)st_.s >= 512 + 12 * nvid::slotBytesFor(fmt));
		// bandwidth ordering: smaller formats are smaller files
		static off_t sizes[3];
		sizes[fmt - 1] = st_.s;
		if (fmt == vfmt::F_RGB332_HALF) CHECK(sizes[2] < sizes[1] && sizes[1] < sizes[0]);
		free(expect);
		free(tmp);

		// ---- a cut-short compact file is still readable by scanning
		{
			int f2 = open(path, O_RDWR);
			CHECK(ftruncate(f2, st_.s - 3000) == 0);
			close(f2);
			// clear the complete flag as a real power cut would leave it
			f2 = open(path, O_RDWR);
			nvid::Header hh;
			CHECK(read(f2, &hh, sizeof hh) == (ssize_t)sizeof hh);
			hh.flags &= ~nvid::FLAG_COMPLETE;
			lseek(f2, 0, SEEK_SET);
			CHECK(write(f2, &hh, sizeof hh) == (ssize_t)sizeof hh);
			close(f2);
			nvid::Reader r2;
			CHECK(r2.open(path));
			CHECK(r2.frameCount() >= 8 && r2.frameCount() <= 12);
		}
	}

	// ---- version 1 files (written before Phase 2D) must still open: same bytes with version = 1
	{
		const char *path = "/tmp/nvid_v1.nvid";
		nvid::Writer w;
		nvid::WriteParams p;
		CHECK(w.open(path, p));
		for (int n = 0; n < 4; n++) {
			makeFrame(src, n);
			memcpy(slot + nvid::VIDEO_FRAME_OFFSET, src, 98304);
			CHECK(w.writeVideoSlot(slot, n * 100));
		}
		nvid::Final fin; fin.durationMs = 400;
		CHECK(w.finish(fin));
		int fd = open(path, O_RDWR);
		nvid::Header h; CHECK(read(fd, &h, sizeof h) == (ssize_t)sizeof h);
		h.version = 1;
		lseek(fd, 0, SEEK_SET); CHECK(write(fd, &h, sizeof h) == (ssize_t)sizeof h);
		close(fd);
		nvid::Reader r;
		CHECK(r.open(path));
		CHECK(r.frameCount() == 4 && r.payloadBytes() == 98304);
		CHECK(r.readFrame(2, out));
		makeFrame(src, 2);
		CHECK(memcmp(out, src, 98304) == 0);

		// a version 1 header claiming RGB332, a bad size, an unknown version and an unknown format are all rejected
		fd = open(path, O_RDWR);
		h.version = 1; h.videoFormat = vfmt::F_RGB332; h.width = 256; h.height = 192;
		lseek(fd, 0, SEEK_SET); CHECK(write(fd, &h, sizeof h) == (ssize_t)sizeof h);
		CHECK(!nvid::validHeader(h));
		h.version = 2; CHECK(nvid::validHeader(h));
		h.width = 128; CHECK(!nvid::validHeader(h));
		h.width = 256; h.version = 3; CHECK(!nvid::validHeader(h));
		h.version = 2; h.videoFormat = 9; CHECK(!nvid::validHeader(h));
		close(fd);
	}

	free(src); free(out); free(slot);
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("container v2 tests OK\n");
	return 0;
}
