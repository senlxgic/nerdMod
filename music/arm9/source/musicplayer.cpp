#include "musicplayer.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#include "common/nmid3.h"
#include "common/nmmp3.h"
#include "common/nmwav.h"
#include "msclock.h"
#include "musicmath.h"

namespace mplay {

namespace {

constexpr u32 RING = 16384;		// frames per channel (32 KiB each, 0.37 s at 44.1 kHz)
constexpr u32 MARGIN = 1024;	// frames already latched by the hardware
constexpr u32 PREFILL = 6144;	// frames buffered before the channels start
constexpr u32 BLOCK = 1152;		// the most one decode step writes
constexpr u32 IN_BYTES = 16384; // MP3 read window
constexpr u32 TAIL = 2048;		// silence after the last sample, so the looping ring never replays old audio

s16 *ringL = nullptr, *ringR = nullptr;
int chL = -1, chR = -1;
FILE *fp = nullptr;
nmmusic::Track cur;
State st = State::Idle;
const char *err = "ok";

u32 rate = 0, period = 0;
u64 wf = 0;			// frames written since the ring origin
u64 base = 0;		// media frame that the ring origin corresponds to
u64 accum = 0;		// frames played before startTicks
u32 startTicks = 0;
u64 endFrames = 0;	// wf when the stream ended (valid when tailed)
bool eof = false, fileEof = false, tailed = false;
u32 under = 0;
int vol = 96;
int lvlL = 0, lvlR = 0;
u32 fileSize = 0;

// MP3
mp3dec_t dec;
u8 *in = nullptr;
int inLen = 0, inPos = 0;
mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
long mp3Off = 0;

// WAV
nmwav::Info wav;
u8 wavBuf[2048];
s16 wavPcm[1024];

u64 playedFrames() {
	if (st == State::Playing)
		return accum + musicmath::framesFromTicks(msclock::ticks() - startTicks, period);
	return accum;
}

void killChannels() {
	if (chL >= 0)
		soundKill(chL);
	if (chR >= 0)
		soundKill(chR);
	chL = chR = -1;
}

void clearRings() {
	if (!ringL || !ringR)
		return;
	memset(ringL, 0, RING * 2);
	memset(ringR, 0, RING * 2);
	DC_FlushRange(ringL, RING * 2);
	DC_FlushRange(ringR, RING * 2);
}

void release() {
	killChannels();
	if (fp) {
		fclose(fp);
		fp = nullptr;
	}
	free(ringL);
	free(ringR);
	free(in);
	ringL = ringR = nullptr;
	in = nullptr;
}

bool startChannels() {
	soundEnable(); // powers the sound hardware and sets the master volume
	chL = soundPlaySample(ringL, SoundFormat_16Bit, RING * 2, rate, vol, 0, true, 0);
	chR = soundPlaySample(ringR, SoundFormat_16Bit, RING * 2, rate, vol, 127, true, 0);
	if (chL < 0 || chR < 0) {
		killChannels();
		err = "no free sound channel";
		st = State::Error;
		return false;
	}
	accum = 0;
	startTicks = msclock::ticks();
	st = State::Playing;
	return true;
}

// interleaved (ch = 1 or 2) -> the two rings
void writeFrames(const s16 *src, int ch, u32 n) {
	int pl = 0, pr = 0;
	u32 idx = (u32)(wf % RING);
	for (u32 i = 0; i < n; i++) {
		const s16 l = src[i * ch];
		const s16 r = ch == 2 ? src[i * ch + 1] : l;
		ringL[idx] = l;
		ringR[idx] = r;
		const int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
		if (al > pl)
			pl = al;
		if (ar > pr)
			pr = ar;
		if (++idx == RING)
			idx = 0;
	}
	// flush the touched part (one or two runs)
	const u32 first = (u32)(wf % RING);
	if (first + n <= RING) {
		DC_FlushRange(ringL + first, n * 2);
		DC_FlushRange(ringR + first, n * 2);
	} else {
		DC_FlushRange(ringL + first, (RING - first) * 2);
		DC_FlushRange(ringR + first, (RING - first) * 2);
		DC_FlushRange(ringL, (first + n - RING) * 2);
		DC_FlushRange(ringR, (first + n - RING) * 2);
	}
	wf += n;
	lvlL = pl;
	lvlR = pr;
}

void writeSilence(u32 n) {
	static s16 zeros[TAIL * 2];
	if (n > TAIL)
		n = TAIL;
	writeFrames(zeros, 2, n);
}

void refillInput() {
	if (inPos > 0) {
		memmove(in, in + inPos, (size_t)(inLen - inPos));
		inLen -= inPos;
		inPos = 0;
	}
	if (fileEof)
		return;
	const size_t n = fread(in + inLen, 1, IN_BYTES - (size_t)inLen, fp);
	inLen += (int)n;
	if (n == 0)
		fileEof = true;
}

// One MP3 frame -> rings. Returns frames written (0 at the end of the stream).
u32 decodeMp3() {
	for (int tries = 0; tries < 48; tries++) {
		if (!fileEof && inLen - inPos < 4096)
			refillInput();
		if (inLen - inPos <= 0) {
			eof = true;
			return 0;
		}
		mp3dec_frame_info_t info;
		const int s = mp3dec_decode_frame(&dec, in + inPos, inLen - inPos, pcm, &info);
		if (info.frame_bytes <= 0) { // no frame in what is left of the window
			if (fileEof) {
				eof = true;
				return 0;
			}
			if (inLen - inPos >= (int)IN_BYTES - 16)
				inPos = inLen; // a whole window of junk: drop it
			else
				refillInput();
			continue;
		}
		inPos += info.frame_bytes;
		if (s > 0 && (info.channels == 1 || info.channels == 2)) {
			writeFrames(pcm, info.channels, (u32)s);
			return (u32)s;
		}
	}
	err = "no decodable audio";
	eof = true;
	return 0;
}

u32 wavPos = 0;

u32 decodeWavBlock() {
	const u32 fb = wav.frameBytes();
	const u32 total = wav.frames();
	if (!fb || wavPos >= total) {
		eof = true;
		return 0;
	}
	u32 n = sizeof(wavBuf) / fb;
	if (n > 512)
		n = 512;
	if (n > total - wavPos)
		n = total - wavPos;
	const size_t got = fread(wavBuf, 1, (size_t)n * fb, fp);
	n = (u32)(got / fb);
	if (!n) {
		eof = true;
		return 0;
	}
	nmwav::toStereo16(wav, wavBuf, n, wavPcm);
	writeFrames(wavPcm, 2, n);
	wavPos += n;
	return n;
}

// Locates the first MP3 frame at or after `from` (absolute offset). Returns -1 when none.
long locateMp3(long from) {
	if (fseek(fp, from, SEEK_SET) != 0)
		return -1;
	const size_t n = fread(in, 1, IN_BYTES, fp);
	if (n < 8)
		return -1;
	nmmp3::Frame f;
	long off = nmmp3::firstFrame(in, n, f);
	if (off >= 0)
		return from + off;
	if (from == 0) { // a large ID3v2 tag (cover art) can hide the first frame: look behind it
		const uint32_t sz = nmid3::v2Size(in, n);
		if (sz >= n && sz < fileSize)
			return locateMp3((long)sz);
	}
	return -1;
}

// Resets the decoder and the rings and starts buffering at media time ms.
bool beginStream(u32 ms) {
	killChannels();
	eof = fileEof = tailed = false;
	wf = accum = 0;
	endFrames = 0;
	clearRings();
	base = (u64)ms * rate / 1000;
	if (cur.kind == nmmusic::Kind::Wav) {
		u32 f = (u32)base;
		if (f >= wav.frames())
			f = 0, base = 0;
		wavPos = f;
		if (fseek(fp, (long)(wav.dataOffset + (u64)f * wav.frameBytes()), SEEK_SET) != 0) {
			err = "seek failed";
			st = State::Error;
			return false;
		}
	} else {
		long pos = mp3Off;
		if (ms > 0 && cur.durationMs > 0) {
			const long target = mp3Off + (long)((u64)(fileSize - (u32)mp3Off) * ms / cur.durationMs);
			const long at = locateMp3(target);
			if (at >= 0)
				pos = at;
			else
				base = 0; // no frame found there: start over from the beginning
		}
		if (fseek(fp, pos, SEEK_SET) != 0) {
			err = "seek failed";
			st = State::Error;
			return false;
		}
		inLen = inPos = 0;
		mp3dec_init(&dec);
	}
	st = State::Buffering;
	return true;
}

} // namespace

bool open(const nmmusic::Track &t, u32 startMs) {
	release();
	st = State::Idle;
	err = "ok";
	cur = t;
	under = 0;
	lvlL = lvlR = 0;
	fp = fopen(t.path, "rb");
	if (!fp) {
		err = "cannot open the file";
		st = State::Error;
		return false;
	}
	fseek(fp, 0, SEEK_END);
	const long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	fileSize = sz > 0 ? (u32)sz : 0;
	ringL = (s16 *)memalign(32, RING * 2);
	ringR = (s16 *)memalign(32, RING * 2);
	in = (u8 *)malloc(IN_BYTES);
	if (!ringL || !ringR || !in) {
		release();
		err = "out of memory";
		st = State::Error;
		return false;
	}
	if (t.kind == nmmusic::Kind::Wav) {
		const size_t n = fread(in, 1, 4096, fp);
		wav = nmwav::parse(in, n, fileSize);
		if (!wav.ok) {
			err = wav.error;
			release();
			st = State::Error;
			return false;
		}
		if (wav.rate < 3000 || wav.rate > 48000) {
			err = "unsupported sample rate";
			release();
			st = State::Error;
			return false;
		}
		rate = wav.rate;
		cur.durationMs = wav.durationMs();
	} else if (t.kind == nmmusic::Kind::Mp3) {
		mp3Off = locateMp3(0);
		if (mp3Off < 0) {
			err = "no MP3 audio found";
			release();
			st = State::Error;
			return false;
		}
		fseek(fp, mp3Off, SEEK_SET);
		const size_t n = fread(in, 1, 64, fp);
		nmmp3::Frame f = nmmp3::header(in);
		if (n < 4 || !f.ok) {
			err = "bad MP3 header";
			release();
			st = State::Error;
			return false;
		}
		rate = (u32)f.rate;
	} else {
		err = "unsupported format";
		release();
		st = State::Error;
		return false;
	}
	period = musicmath::periodFor(rate);
	if (!period) {
		err = "bad sample rate";
		release();
		st = State::Error;
		return false;
	}
	if (!beginStream(startMs))
		return false;
	update();
	return true;
}

void update() {
	if (st == State::Idle || st == State::Error || st == State::Finished || st == State::Paused)
		return;
	u64 p = playedFrames();
	if (st == State::Playing && !eof && musicmath::underrun(wf, p, 64)) {
		under++; // the decoder fell behind: restart the output from what was decoded
		killChannels();
		base += wf;
		wf = accum = 0;
		clearRings();
		st = State::Buffering;
		p = 0;
	}
	int steps = 0;
	while (!eof && steps < 6 && musicmath::ringFree(wf, p, RING, MARGIN) >= BLOCK) {
		if (cur.kind == nmmusic::Kind::Wav)
			decodeWavBlock();
		else
			decodeMp3();
		steps++;
	}
	if (eof && !tailed && musicmath::ringFree(wf, p, RING, MARGIN) >= TAIL) {
		endFrames = wf;
		writeSilence(TAIL);
		tailed = true;
	}
	if (st == State::Buffering) {
		if (tailed && endFrames == 0) { // nothing could be decoded
			if (err[0] == 'o' && err[1] == 'k')
				err = "empty or unreadable track";
			st = State::Finished;
			return;
		}
		if (wf >= PREFILL || tailed)
			startChannels();
	} else if (st == State::Playing && tailed && p >= endFrames) {
		killChannels();
		st = State::Finished;
	}
}

void pause() {
	if (st != State::Playing)
		return;
	accum = playedFrames();
	st = State::Paused;
	if (chL >= 0)
		soundPause(chL);
	if (chR >= 0)
		soundPause(chR);
}

void resume() {
	if (st != State::Paused)
		return;
	startTicks = msclock::ticks();
	st = State::Playing;
	if (chL >= 0)
		soundResume(chL);
	if (chR >= 0)
		soundResume(chR);
}

void togglePause() {
	if (st == State::Playing)
		pause();
	else if (st == State::Paused)
		resume();
}

void seekPermille(u32 permille) {
	if (!fp || cur.durationMs == 0 || st == State::Idle || st == State::Error)
		return;
	if (permille > 1000)
		permille = 1000;
	beginStream((u32)((u64)cur.durationMs * permille / 1000));
}

void seekRelativeMs(int deltaMs) {
	if (!fp || cur.durationMs == 0 || st == State::Idle || st == State::Error)
		return;
	long t = (long)positionMs() + deltaMs;
	if (t < 0)
		t = 0;
	if ((u32)t >= cur.durationMs)
		t = (long)cur.durationMs - 1000 > 0 ? (long)cur.durationMs - 1000 : 0;
	beginStream((u32)t);
}

void stop() {
	release();
	st = State::Idle;
}

void setVolume(int v) {
	vol = v < 0 ? 0 : (v > 127 ? 127 : v);
	if (chL >= 0)
		soundSetVolume(chL, (u8)vol);
	if (chR >= 0)
		soundSetVolume(chR, (u8)vol);
}

int volume() { return vol; }
State state() { return st; }

u32 positionMs() {
	if (st == State::Idle || st == State::Error)
		return 0;
	u64 p = playedFrames();
	if (tailed && p > endFrames)
		p = endFrames;
	u32 ms = musicmath::mediaMs(base, p, rate);
	if (cur.durationMs && ms > cur.durationMs)
		ms = cur.durationMs;
	return ms;
}

u32 durationMs() { return cur.durationMs; }
u32 sampleRate() { return rate; }
u32 underruns() { return under; }

void levels(int &l, int &r) {
	l = lvlL;
	r = lvlR;
}

const char *lastError() { return err; }

const char *stateName() {
	switch (st) {
		case State::Buffering: return "Buffering...";
		case State::Playing: return "Playing";
		case State::Paused: return "Paused";
		case State::Finished: return "Finished";
		case State::Error: return "Error";
		default: return "Stopped";
	}
}

} // namespace mplay
