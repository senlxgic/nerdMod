// Host-side test of videoContainer.cpp (Writer / Reader), including cut-short and damaged files.
//   make -C camera/tools/hosttest        (needs g++ only)
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "../../arm9/source/videoContainer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static u8 *slot() { u8 *p = (u8 *)aligned_alloc(32, 32 + nvid::FRAME_BYTES); memset(p, 0, 32 + nvid::FRAME_BYTES); return p; }

static void fillFrame(u8 *s, int n) {
	u16 *px = (u16 *)(s + 32);
	for (u32 i = 0; i < nvid::WIDTH * nvid::HEIGHT; i++) px[i] = (u16)(0x8000 | ((n * 977 + i) & 0x7FFF));
}

static bool frameMatches(const u16 *px, int n) {
	for (u32 i = 0; i < nvid::WIDTH * nvid::HEIGHT; i++)
		if (px[i] != (u16)(0x8000 | ((n * 977 + i) & 0x7FFF))) return false;
	return true;
}

static void writeFile(const char *path, int frames, bool audio, bool finish) {
	nvid::Writer w;
	nvid::WriteParams p; p.audio = audio; p.innerCamera = true;
	CHECK(w.open(path, p));
	u8 *s = slot();
	u8 *a = (u8 *)aligned_alloc(32, 32 + 4096);
	for (int n = 0; n < frames; n++) {
		fillFrame(s, n);
		CHECK(w.writeVideo(s + 16, nvid::FRAME_BYTES, n * 100));
		if (audio && n % 5 == 4) {
			memset(a + 32, n, 4096);
			CHECK(w.writeAudio(a + 16, 4096, n * 100));
		}
	}
	if (finish) {
		nvid::Final f; f.durationMs = frames * 100; f.droppedFrames = 2;
		CHECK(w.finish(f));
	} else {
		w.abandon();
	}
	free(s); free(a);
}

int main() {
	const char *path = "/tmp/nvid_test.nvid";

	// 1. clean file
	writeFile(path, 25, true, true);
	{
		nvid::Reader r;
		CHECK(r.open(path));
		CHECK(r.frameCount() == 25);
		CHECK(r.hasAudio());
		CHECK(r.durationMs() == 2500);
		CHECK(r.info().flags & nvid::FLAG_COMPLETE);
		CHECK(r.info().flags & nvid::FLAG_INNER_CAMERA);
		CHECK(r.info().flags & nvid::FLAG_FRAMES_DROPPED);
		u16 *buf = (u16 *)aligned_alloc(32, nvid::FRAME_BYTES);
		u32 t = 0;
		CHECK(r.readFrame(0, buf, &t) && t == 0 && frameMatches(buf, 0));
		CHECK(r.readFrame(24, buf, &t) && t == 2400 && frameMatches(buf, 24));
		CHECK(!r.readFrame(25, buf));
		// sequential walk: 25 video + 5 audio chunks, in order
		CHECK(r.rewind());
		int v = 0, au = 0; nvid::Reader::Chunk c;
		while (r.nextChunk(c)) {
			if (c.fourcc == nvid::CHUNK_VIDEO) { CHECK(c.size == nvid::FRAME_BYTES); CHECK(r.readPayload(buf, c.size)); CHECK(frameMatches(buf, v)); v++; }
			else { CHECK(c.fourcc == nvid::CHUNK_AUDIO); CHECK(r.skipPayload(c.size)); au++; }
		}
		CHECK(v == 25 && au == 5);
		// seek
		CHECK(r.seekToTime(1230));
		CHECK(r.nextChunk(c) && c.fourcc == nvid::CHUNK_VIDEO && c.timeMs == 1200);
		free(buf);
	}

	// 2. cut short in the middle of a frame: readable by scanning, last partial frame ignored
	{
		writeFile(path, 10, true, false);
		off_t full = lseek(open(path, O_RDONLY), 0, SEEK_END);
		CHECK(truncate(path, full - 5000) == 0);
		nvid::Reader r;
		CHECK(r.open(path));
		CHECK(r.frameCount() == 9);
		CHECK(!(r.info().flags & nvid::FLAG_COMPLETE));
		u16 *buf = (u16 *)aligned_alloc(32, nvid::FRAME_BYTES);
		CHECK(r.readFrame(8, buf) && frameMatches(buf, 8));
		free(buf);
	}

	// 3. damaged header / not our file
	{
		FILE *f = fopen(path, "wb"); fwrite("hello world, not a video at all, padding padding padding padding padding", 1, 72, f); fclose(f);
		nvid::Reader r;
		CHECK(!r.open(path));
		CHECK(!r.open("/tmp/does_not_exist.nvid"));
	}

	// 4. complete flag set but index pointing outside the file: falls back to scanning
	{
		writeFile(path, 6, false, true);
		int fd = open(path, O_RDWR);
		nvid::Header h; CHECK(read(fd, &h, sizeof(h)) == sizeof(h));
		h.indexOffset = 0x7FFFFFF0;
		lseek(fd, 0, SEEK_SET); CHECK(write(fd, &h, sizeof(h)) == sizeof(h)); close(fd);
		nvid::Reader r;
		CHECK(r.open(path));
		CHECK(r.frameCount() == 6);
	}

	// 5. a file written by tools/nerdvid-convert (if present)
	if (access("/tmp/t.nvid", R_OK) == 0) {
		nvid::Reader r;
		CHECK(r.open("/tmp/t.nvid"));
		CHECK(r.frameCount() == 30 && r.hasAudio());
		u16 *buf = (u16 *)aligned_alloc(32, nvid::FRAME_BYTES);
		CHECK(r.readFrame(29, buf));
		free(buf);
		printf("converter file: ok\n");
	}

	remove(path);
	printf(failures ? "%d FAILURES\n" : "all container tests passed\n", failures);
	return failures ? 1 : 0;
}
