#include "video.h"

#include <cerrno>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audioRecorder.h"
#include "fpsutil.h"
#include "common/systemdetails.h"
#include "msclock.h"
#include "videoContainer.h"

namespace rec {

namespace {

// ---- buffers -------------------------------------------------------------------------------------
// A slot is 16 bytes of padding, the 16-byte chunk header and the frame. Header + frame are written in one go.
constexpr u32 SLOT_PAD = 16;
constexpr u32 SLOT_BYTES = 32 + nvid::FRAME_BYTES; // 98336, a multiple of 32
constexpr int SLOT_COUNT = 12;					   // maximum; 1 being captured + up to 11 waiting for the card (1.15 MB; 0.37 s at 30 fps)
constexpr u32 AUDIO_CHUNK_MAX = 16384;
constexpr u32 AUDIO_CHUNK_MIN = 8192;			   // write audio as soon as this much is waiting (0.25 s)
constexpr u32 AUDIO_URGENT = 24576;				   // ... and before video if this much is waiting (0.75 s)

constexpr int MAX_CONSECUTIVE_SKIPS = 30;		   // about 1 second of frames with no free buffer
constexpr u32 SLOW_WRITE_MS = 500;
constexpr int MAX_SLOW_WRITES = 6;

struct Slot {
	u8 *base = nullptr;
	u16 *pixels() const { return (u16 *)(base + 32); }
	u8 *chunk() const { return base + SLOT_PAD; }
};

Slot slots[SLOT_COUNT];
u8 freeList[SLOT_COUNT];
int freeCount = 0;
int slotN = SLOT_COUNT;		// buffers actually allocated (at least MIN_SLOTS)
constexpr int MIN_SLOTS = 4;
u8 queueSlot[SLOT_COUNT];
u32 queueTime[SLOT_COUNT];
int queueHead = 0, queueCount = 0;
int current = -1;
int shown = -1;			// slot that lastFrame() refers to (valid until the next frameCaptured)

u8 *audioChunk = nullptr;
bool audioOn = false;
u32 audioPosBytes = 0;	// bytes of audio written so far (the time base of audio chunks)

nvid::Writer writer;
bool recording = false;
bool innerCam = false;
StopReason pending = STOP_NONE;
std::string tmpPath;

int fpsSetting = fpsutil::DEFAULT_FPS;
int recFps = fpsutil::DEFAULT_FPS;
u32 capturedCount = 0, storedCount = 0, bufferPeak = 0;
u64 writeMsSum = 0;
u32 writeCount = 0, writeMsMax = 0;

u32 startTicks = 0;
u32 nextIndex = 0;			// next frame slot (frame k is due at fpsutil::dueMs(k))
int consecutiveSkips = 0;
u32 skippedDue = 0;
int slowWrites = 0;

// ---- clock -----------------------------------------------------------------------------------------
u32 nowMs() { return msclock::toMs(msclock::ticks() - startTicks); }

// ---- helpers ---------------------------------------------------------------------------------------
void freeBuffers() {
	for (int i = 0; i < SLOT_COUNT; i++) {
		free(slots[i].base);
		slots[i].base = nullptr;
	}
	free(audioChunk);
	audioChunk = nullptr;
	freeCount = 0;
	queueHead = queueCount = 0;
	current = shown = -1;
}

bool allocBuffers() {
	slotN = 0;
	for (int i = 0; i < SLOT_COUNT; i++) {
		slots[i].base = (u8 *)memalign(32, SLOT_BYTES);
		if (!slots[i].base)
			break;
		slotN++;
	}
	if (slotN < MIN_SLOTS) {
		freeBuffers();
		return false;
	}
	audioChunk = (u8 *)memalign(32, 32 + AUDIO_CHUNK_MAX);
	if (!audioChunk) {
		freeBuffers();
		return false;
	}
	freeCount = 0;
	for (int i = slotN - 1; i >= 1; i--)
		freeList[freeCount++] = (u8)i;
	current = 0;
	shown = -1;
	queueHead = queueCount = 0;
	return true;
}

std::string deviceRoot() { return sys().isRunFromSD() ? "sd:" : "fat:"; }

void noteWriteError() {
	if (pending != STOP_NONE)
		return;
	pending = (writer.lastError() == ENOSPC) ? STOP_SD_FULL : STOP_WRITE_ERROR;
}

// Writes one queued video frame. Returns false on a write error.
bool writeOneVideo() {
	const int s = queueSlot[queueHead];
	const u32 t = queueTime[queueHead];
	queueHead = (queueHead + 1) % slotN;
	queueCount--;

	const u32 before = msclock::ticks();
	const bool ok = writer.writeVideo(slots[s].chunk(), nvid::FRAME_BYTES, t);
	const u32 ms = msclock::toMs(msclock::ticks() - before);
	if (ms > SLOW_WRITE_MS)
		slowWrites++;
	writeMsSum += ms;
	writeCount++;
	if (ms > writeMsMax)
		writeMsMax = ms;
	if (ok)
		storedCount++;

	freeList[freeCount++] = (u8)s;
	if (!ok)
		noteWriteError();
	return ok;
}

bool writeOneAudio(u32 maxBytes) {
	u8 *payload = audioChunk + 32;
	u32 n = audioRec::read(payload, maxBytes & ~15u); // multiples of 16 bytes keep the container aligned
	if (!n)
		return true;
	const bool ok = writer.writeAudio(audioChunk + 16, n, audioPosBytes / 32);
	audioPosBytes += n;
	if (!ok)
		noteWriteError();
	return ok;
}

} // namespace

// =================================================================================================
std::string videoFolder() { return deviceRoot() + "/_nds/nerdMod/videos"; }

bool ensureVideoFolder() {
	const std::string root = deviceRoot();
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir((root + "/_nds/nerdMod").c_str(), 0777);
	mkdir(videoFolder().c_str(), 0777);
	struct stat st;
	return stat(videoFolder().c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool active() { return recording; }

bool start(bool innerCamera, std::string &error) {
	if (recording)
		return true;
	if (!sys().fatInitOk() || !ensureVideoFolder()) {
		error = "No SD card";
		return false;
	}
	if (!allocBuffers()) {
		error = "Out of memory";
		return false;
	}

	tmpPath = videoFolder() + "/REC_TEMP.nvid.tmp";
	remove(tmpPath.c_str()); // a leftover from an interrupted recording

	audioOn = audioRec::start();

	nvid::WriteParams params;
	params.innerCamera = innerCamera;
	params.audio = audioOn;
	params.audioRate = (u16)audioRec::SAMPLE_RATE;
	recFps = fpsutil::sanitize(fpsSetting);
	params.fpsNum = (u16)recFps;
	params.startUnix = (u32)time(NULL);
	if (!writer.open(tmpPath, params)) {
		error = (writer.lastError() == ENOSPC) ? "SD card full" : "Cannot create file";
		audioRec::release();
		freeBuffers();
		audioOn = false;
		return false;
	}

	innerCam = innerCamera;
	pending = STOP_NONE;
	nextIndex = 0;
	consecutiveSkips = 0;
	skippedDue = 0;
	slowWrites = 0;
	capturedCount = storedCount = bufferPeak = 0;
	writeMsSum = 0;
	writeCount = writeMsMax = 0;
	audioPosBytes = 0;
	msclock::start();
	startTicks = msclock::ticks();
	recording = true;
	return true;
}

u16 *captureTarget() { return current >= 0 ? slots[current].pixels() : nullptr; }
const u16 *lastFrame() { return shown >= 0 ? slots[shown].pixels() : nullptr; }
u32 elapsedMs() { return recording ? nowMs() : 0; }
u32 queuedFrames() { return (u32)queueCount; }
u32 skippedDueFrames() { return skippedDue; }
bool hasMicData() { return audioOn && audioRec::gotData(); }
void setFps(int f) {
	if (!recording)
		fpsSetting = fpsutil::sanitize(f);
}
int fps() { return recording ? recFps : fpsSetting; }
int slotCount() { return slotN; }

Stats stats() {
	Stats s;
	s.fps = fps();
	s.elapsedMs = elapsedMs();
	s.captured = capturedCount;
	s.stored = storedCount;
	const u32 expected = recording ? fpsutil::expectedFrames(s.elapsedMs, recFps) : 0;
	const u32 have = storedCount + (u32)queueCount + 1; // +1: the frame being captured right now
	s.dropped = expected > have ? expected - have : 0;
	s.queued = (u32)queueCount;
	s.bufferPeak = bufferPeak;
	s.sdAvgMs = writeCount ? (u32)(writeMsSum / writeCount) : 0;
	s.sdMaxMs = writeMsMax;
	s.capturedFpsX100 = fpsutil::averageFpsX100(capturedCount, s.elapsedMs);
	s.micActive = audioOn;
	s.micData = audioOn && audioRec::gotData();
	return s;
}

void frameCaptured() {
	if (!recording || current < 0)
		return;
	DC_InvalidateRange(slots[current].pixels(), nvid::FRAME_BYTES); // the camera DMA wrote this behind the cache
	shown = current;

	capturedCount++;
	const u32 now = nowMs();
	// a frame is due a little before its exact time (fpsutil::slackMs), so camera jitter does not skip slots
	if (!fpsutil::isDue(now, nextIndex, recFps))
		return; // not due: the same buffer is reused for the next frame

	if (freeCount == 0) {
		skippedDue++;
		if (++consecutiveSkips >= MAX_CONSECUTIVE_SKIPS && pending == STOP_NONE)
			pending = STOP_TOO_SLOW;
		return;
	}
	consecutiveSkips = 0;

	queueSlot[(queueHead + queueCount) % slotN] = (u8)current;
	queueTime[(queueHead + queueCount) % slotN] = now;
	queueCount++;
	if ((u32)queueCount > bufferPeak)
		bufferPeak = (u32)queueCount;
	current = freeList[--freeCount];

	u32 k = fpsutil::nearestIndex(now, recFps);
	if (k < nextIndex)
		k = nextIndex;
	nextIndex = k + 1;
}

void pump() {
	if (!recording || pending != STOP_NONE)
		return;

	const u32 audioWaiting = audioOn ? audioRec::available() : 0;
	bool ok = true;
	if (audioWaiting >= AUDIO_URGENT || (queueCount == 0 && audioWaiting >= AUDIO_CHUNK_MIN)) {
		ok = writeOneAudio(AUDIO_CHUNK_MAX);
	} else if (queueCount > 0) {
		ok = writeOneVideo();
	}
	(void)ok;

	if (pending == STOP_NONE) {
		if (slowWrites >= MAX_SLOW_WRITES)
			pending = STOP_TOO_SLOW;
		else if (writer.bytesWritten() >= MAX_FILE_BYTES || nowMs() >= MAX_DURATION_MS)
			pending = STOP_LIMIT;
	}
}

StopReason autoStopReason() { return pending; }

Result stop(StopReason reason) {
	Result r;
	r.reason = (pending != STOP_NONE && reason == STOP_USER) ? pending : reason;
	if (!recording)
		return r;

	const u32 endMs = nowMs();
	audioRec::stop(); // no more callbacks; the ring keeps what is left
	r.hasAudio = audioOn && audioRec::gotData();

	// Write out what is queued. On a write error give up on the rest (the file is still finalised below).
	bool ok = (pending != STOP_SD_FULL && pending != STOP_WRITE_ERROR);
	while (ok && queueCount > 0)
		ok = writeOneVideo();
	while (ok && audioOn && audioRec::available() >= 16)
		ok = writeOneAudio(AUDIO_CHUNK_MAX);

	nvid::Final fin;
	// frames that were due in the recorded time but are not in the file
	const u32 expected = fpsutil::expectedFrames(endMs, recFps);
	fin.droppedFrames = expected > writer.frames() ? expected - writer.frames() : 0;
	fin.framesDropped = fin.droppedFrames > 2;
	fin.durationMs = endMs;
	fin.audioFailed = audioOn && !r.hasAudio;
	fin.maxWriteMs = writeMsMax;
	fin.capturedFrames = capturedCount;
	const u32 frames = writer.frames();
	const bool finished = writer.finish(fin);
	(void)finished;

	audioRec::release();
	msclock::stop();
	freeBuffers();
	recording = false;
	audioOn = false;
	if (pending != STOP_NONE && r.reason == STOP_USER)
		r.reason = pending;

	r.frames = frames;
	r.droppedFrames = fin.droppedFrames;
	r.durationMs = endMs;
	r.requestedFps = recFps;
	r.avgFpsX100 = fpsutil::averageFpsX100(frames, endMs);
	r.capturedFrames = capturedCount;
	r.maxWriteMs = writeMsMax;
	r.avgWriteMs = writeCount ? (u32)(writeMsSum / writeCount) : 0;
	r.bufferPeak = bufferPeak;

	if (frames == 0) {
		remove(tmpPath.c_str()); // nothing worth keeping
		return r;
	}

	// Final name
	time_t t = time(NULL);
	struct tm *tmv = localtime(&t);
	char base[40];
	if (tmv)
		snprintf(base, sizeof(base), "NV_%04d%02d%02d_%02d%02d%02d", tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
	else
		snprintf(base, sizeof(base), "NV_00000000_%06lu", (unsigned long)(t & 0xFFFFFF));
	std::string name = std::string(base) + ".nvid";
	for (int n = 1; n < 1000; n++) {
		struct stat st;
		if (stat((videoFolder() + "/" + name).c_str(), &st) != 0)
			break;
		char suffix[12];
		snprintf(suffix, sizeof(suffix), "_%d", n);
		name = std::string(base) + suffix + ".nvid";
	}
	if (rename(tmpPath.c_str(), (videoFolder() + "/" + name).c_str()) == 0) {
		r.saved = true;
		r.name = name;
	} else {
		// keep the data under its temp name; the user will not see it in the album, but nothing is deleted
		r.saved = false;
	}
	return r;
}

} // namespace rec
