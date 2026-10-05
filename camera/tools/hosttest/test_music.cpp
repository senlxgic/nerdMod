// Host tests for the Music app cores: WAV parser, ID3, MP3 headers + a real minimp3 decode, library index.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#include "../../../music/arm9/source/minimp3.h"

#include "../../../universal/include/common/nmid3.h"
#include "../../../universal/include/common/nmmp3.h"
#include "../../../universal/include/common/nmmusic.h"
#include "../../../universal/include/common/nmwav.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> readFile(const char *p) {
	std::vector<uint8_t> v;
	FILE *f = fopen(p, "rb");
	if (!f) return v;
	uint8_t b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
	fclose(f);
	return v;
}

static void le32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 255); }
static void le16(std::vector<uint8_t> &v, uint32_t x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); }

static std::vector<uint8_t> makeWav(int ch, int bits, int rate, int frames, int tag = 1) {
	std::vector<uint8_t> v;
	const uint32_t dataBytes = frames * ch * bits / 8;
	v.insert(v.end(), {'R', 'I', 'F', 'F'}); le32(v, 36 + dataBytes); v.insert(v.end(), {'W', 'A', 'V', 'E'});
	v.insert(v.end(), {'f', 'm', 't', ' '}); le32(v, 16); le16(v, tag); le16(v, ch); le32(v, rate); le32(v, rate * ch * bits / 8); le16(v, ch * bits / 8); le16(v, bits);
	v.insert(v.end(), {'L', 'I', 'S', 'T'}); le32(v, 5); v.insert(v.end(), {'a', 'b', 'c', 'd', 'e', 0}); // odd chunk with pad byte
	v.insert(v.end(), {'d', 'a', 't', 'a'}); le32(v, dataBytes);
	for (int i = 0; i < frames * ch; i++) {
		if (bits == 16) le16(v, (uint16_t)(int16_t)(i * 100 - 2000)); else v.push_back((uint8_t)(128 + (i % 50) - 25));
	}
	return v;
}

static void testWav() {
	for (int ch = 1; ch <= 2; ch++)
		for (int bits = 8; bits <= 16; bits += 8) {
			auto w = makeWav(ch, bits, 22050, 100);
			nmwav::Info i = nmwav::parse(w.data(), w.size() < 4096 ? w.size() : 4096, (uint32_t)w.size());
			CHECK(i.ok && i.channels == ch && i.bits == bits && i.rate == 22050 && i.frames() == 100);
			CHECK(i.dataOffset == 12 + 24 + 14 + 8);
			CHECK(i.durationMs() == 4); // 100 frames at 22050 Hz
			std::vector<int16_t> out(200);
			nmwav::toStereo16(i, w.data() + i.dataOffset, 100, out.data());
			if (bits == 16 && ch == 1) CHECK(out[0] == out[1] && out[0] == -2000 && out[2] == -1900);
			if (bits == 8 && ch == 2) CHECK(out[0] == ((int)w[i.dataOffset] - 128) * 256 && out[1] == ((int)w[i.dataOffset + 1] - 128) * 256);
		}
	// truncated file: the data chunk claims more than exists
	auto w = makeWav(2, 16, 44100, 100);
	w.resize(w.size() - 40);
	nmwav::Info t = nmwav::parse(w.data(), w.size(), (uint32_t)w.size());
	CHECK(t.ok && t.frames() == 90);
	// not PCM / garbage / too short
	auto f = makeWav(2, 16, 44100, 10, 3);
	CHECK(!nmwav::parse(f.data(), f.size(), (uint32_t)f.size()).ok);
	uint8_t junk[64];
	for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 7);
	CHECK(!nmwav::parse(junk, 64, 64).ok);
	CHECK(!nmwav::parse(junk, 5, 5).ok);
	auto b = makeWav(2, 24, 44100, 10);
	CHECK(!nmwav::parse(b.data(), b.size(), (uint32_t)b.size()).ok);
	// every prefix of a valid file must be rejected or handled without reading past the buffer
	auto g = makeWav(2, 16, 44100, 10);
	for (size_t n = 0; n < g.size(); n++) nmwav::parse(g.data(), n, (uint32_t)g.size());
}

static void testId3() {
	auto s = readFile("fixtures/tone_stereo.mp3");
	CHECK(s.size() > 1000);
	nmid3::Tags t;
	CHECK(nmid3::parseV2(s.data(), s.size() < 4096 ? s.size() : 4096, t));
	CHECK(!strcmp(t.title, "Test Tone") && !strcmp(t.artist, "nerdMod") && !strcmp(t.album, "Fixtures"));
	auto m = readFile("fixtures/tone_mono.mp3");
	nmid3::Tags t1;
	CHECK(nmid3::v2Size(m.data(), m.size()) == 0);
	CHECK(nmid3::parseV1(m.data() + m.size() - 128, t1));
	CHECK(!strcmp(t1.title, "Mono Tone") && !strcmp(t1.artist, "Old Tagger"));
	// UTF-16 + latin1 handling built by hand (v2.3, TIT2 UTF-16 with BOM)
	std::vector<uint8_t> h = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
	std::vector<uint8_t> fr = {'T', 'I', 'T', '2', 0, 0, 0, 0, 0, 0, 1, 0xFF, 0xFE, 'H', 0, 'i', 0, 0xE9, 0};
	fr[7] = (uint8_t)(fr.size() - 10);
	h.insert(h.end(), fr.begin(), fr.end());
	h[9] = (uint8_t)(h.size() - 10);
	nmid3::Tags u;
	CHECK(nmid3::parseV2(h.data(), h.size(), u) && !strcmp(u.title, "Hi?"));
	// garbage and truncation never crash
	for (size_t n = 0; n <= h.size(); n++) { nmid3::Tags x; nmid3::parseV2(h.data(), n, x); }
	uint8_t bad[32] = {'I', 'D', '3', 4, 0, 0, 0x7F, 0x7F, 0x7F, 0x7F};
	nmid3::Tags x;
	CHECK(!nmid3::parseV2(bad, 32, x) || true);
	// filename fallback
	nmid3::Tags f;
	nmid3::fromFilename("/Music/Some_Artist - A Song.mp3", f);
	CHECK(!strcmp(f.artist, "Some Artist") && !strcmp(f.title, "A Song"));
	nmid3::Tags g;
	nmid3::fromFilename("/Music/track01.wav", g);
	CHECK(!strcmp(g.title, "track01") && !g.artist[0]);
}

static void testMp3() {
	for (const char *name : {"fixtures/tone_stereo.mp3", "fixtures/tone_mono.mp3"}) {
		auto s = readFile(name);
		CHECK(!s.empty());
		nmmp3::Frame f;
		const long off = nmmp3::firstFrame(s.data(), s.size() < 4096 ? s.size() : 4096, f);
		CHECK(off >= 0 && f.ok);
		const uint32_t dur = nmmp3::durationMs(s.data(), s.size() < 4096 ? s.size() : 4096, off, f, (uint32_t)s.size());
		printf("%s: %d Hz %d ch %d kbps, duration %u ms\n", name, f.rate, f.channels, f.bitrateKbps, dur);
		CHECK(dur > 900 && dur < 1200); // generated as one second
		// full decode with minimp3
		mp3dec_t d;
		mp3dec_init(&d);
		static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
		size_t pos = 0;
		long samples = 0;
		int frames = 0;
		double energy = 0;
		while (pos < s.size()) {
			mp3dec_frame_info_t info;
			int n = mp3dec_decode_frame(&d, s.data() + pos, (int)(s.size() - pos), pcm, &info);
			if (!info.frame_bytes) break;
			pos += info.frame_bytes;
			if (n) {
				frames++;
				samples += n;
				CHECK(info.hz == f.rate && info.channels == f.channels);
				for (int i = 0; i < n * info.channels; i++) energy += (double)pcm[i] * pcm[i];
			}
		}
		printf("  decoded %d frames, %ld samples/channel\n", frames, samples);
		CHECK(frames > 20);
		CHECK(samples > f.rate * 9 / 10 && samples < f.rate * 12 / 10);
		CHECK(sqrt(energy / (samples * f.channels)) > 1000); // a loud sine, so the decode is not silence
	}
	// corrupt data must not crash the decoder or the header scan
	auto s = readFile("fixtures/tone_stereo.mp3");
	srand(5);
	for (int round = 0; round < 200; round++) {
		auto c = s;
		for (int k = 0; k < 40; k++) c[rand() % c.size()] = (uint8_t)rand();
		mp3dec_t d;
		mp3dec_init(&d);
		static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
		size_t pos = 0;
		int guard = 0;
		while (pos < c.size() && guard++ < 2000) {
			mp3dec_frame_info_t info;
			mp3dec_decode_frame(&d, c.data() + pos, (int)(c.size() - pos), pcm, &info);
			if (!info.frame_bytes) break;
			pos += info.frame_bytes;
		}
		nmmp3::Frame f;
		nmmp3::firstFrame(c.data(), c.size() < 4096 ? c.size() : 4096, f);
	}
	// random garbage is not an MP3
	uint8_t junk[512];
	for (int i = 0; i < 512; i++) junk[i] = (uint8_t)(i * 31 + 7);
	nmmp3::Frame jf;
	CHECK(nmmp3::firstFrame(junk, 512, jf) < 0);
}

static void testLibrary() {
	CHECK(nmmusic::kindOfPath("/a/b.MP3") == nmmusic::Kind::Mp3);
	CHECK(nmmusic::kindOfPath("/a/b.wav") == nmmusic::Kind::Wav);
	CHECK(nmmusic::kindOfPath("/a/b.txt") == nmmusic::Kind::Unknown);
	CHECK(nmmusic::kindOfPath("/a/b") == nmmusic::Kind::Unknown);
	std::vector<nmmusic::Track> t(4);
	memset(t.data(), 0, t.size() * sizeof(nmmusic::Track));
	auto set = [&](int i, const char *path, const char *title, const char *artist, uint32_t size) {
		snprintf(t[i].path, sizeof t[i].path, "%s", path);
		snprintf(t[i].title, sizeof t[i].title, "%s", title);
		snprintf(t[i].artist, sizeof t[i].artist, "%s", artist);
		t[i].size = size;
		t[i].durationMs = 1000 * (i + 1);
		t[i].kind = nmmusic::kindOfPath(path);
	};
	set(0, "/Music/z.mp3", "Zed", "", 10);
	set(1, "/Music/b.mp3", "Beta", "bob", 20);
	set(2, "/Music/a.wav", "Alpha", "Bob", 30);
	set(3, "/Music/c.mp3", "Charlie", "Al", 40);
	nmmusic::sortTracks(t.data(), 4);
	CHECK(!strcmp(t[0].title, "Charlie") && !strcmp(t[1].title, "Alpha") && !strcmp(t[2].title, "Beta") && !strcmp(t[3].title, "Zed"));
	// cache round trip
	std::string blob;
	for (auto &x : t) { char l[400]; nmmusic::formatLine(x, l, sizeof l); blob += l; }
	std::vector<nmmusic::Track> back;
	size_t p = 0;
	while (p < blob.size()) {
		size_t e = blob.find('\n', p);
		std::string line = blob.substr(p, e - p + 1);
		p = e + 1;
		nmmusic::Track x;
		std::vector<char> buf(line.begin(), line.end());
		buf.push_back(0);
		if (nmmusic::parseLine(buf.data(), x)) back.push_back(x);
	}
	CHECK(back.size() == 4);
	CHECK(!strcmp(back[1].title, "Alpha") && back[1].size == 30 && back[1].durationMs == t[1].durationMs && back[1].kind == nmmusic::Kind::Wav);
	CHECK(nmmusic::findCached(back.data(), 4, "/Music/a.wav", 30) != nullptr);
	CHECK(nmmusic::findCached(back.data(), 4, "/Music/a.wav", 31) == nullptr); // size changed: re-read
	// damaged lines are skipped
	char bad1[] = "no tabs here\n", bad2[] = "/x.mp3\t1\t2\n", bad3[] = "/x.xyz\t1\t2\ta\tb\tc\n";
	nmmusic::Track x;
	CHECK(!nmmusic::parseLine(bad1, x) && !nmmusic::parseLine(bad2, x) && !nmmusic::parseLine(bad3, x));
	char tm[16];
	nmmusic::formatTime(187000, tm, sizeof tm);
	CHECK(!strcmp(tm, "3:07"));
	nmmusic::formatTime(0xFFFFFFFFu, tm, sizeof tm);
	CHECK(!strcmp(tm, "--:--"));
	// shuffle never repeats the current track
	uint32_t rng = 1;
	int cur = 3;
	int seen[8] = {0};
	for (int i = 0; i < 400; i++) { int n = nmmusic::nextIndex(cur, 8, true, rng); CHECK(n != cur && n >= 0 && n < 8); seen[n]++; cur = n; }
	for (int i = 0; i < 8; i++) CHECK(seen[i] > 10);
	CHECK(nmmusic::nextIndex(7, 8, false, rng) == 0 && nmmusic::nextIndex(0, 8, false, rng, -1) == 7);
	CHECK(nmmusic::nextIndex(0, 0, false, rng) == -1);
}

int main() {
	testWav();
	testId3();
	testMp3();
	testLibrary();
	if (fails) { printf("music tests FAILED (%d)\n", fails); return 1; }
	printf("music tests OK\n");
	return 0;
}
