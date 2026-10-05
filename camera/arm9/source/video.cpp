#include "video.h"

#include <cerrno>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audioRecorder.h"
#include "audiofmt.h"
#include "reclog.h"
#include "fpsutil.h"
#include "common/systemdetails.h"
#include "msclock.h"
#include "videoContainer.h"
#include "videofmt.h"

namespace rec {

namespace {

// ---- buffers -------------------------------------------------------------------------------------
// A slot is the sector-aligned NERDVID unit (see videoContainer.h): PAD chunk, VFRM header, frame. The camera DMA
// lands at slot + 512 and the whole slot (98816 bytes = 193 sectors) goes to the card in one write.
constexpr int SLOT_COUNT = 32;					   // maximum number of slots; how many are used depends on the format (SLOT_BUDGET_BYTES)
constexpr u32 SLOT_BUDGET_BYTES = 1200000;		   // HIGH: 12 slots (0.4 s at 30 fps), BALANCED: 24, SMALL: 32
constexpr u32 AUDIO_CHUNK_MAX = 16384;
constexpr u32 AUDIO_CHUNK_MIN = 8192;			   // write audio as soon as this much is waiting (0.25 s)
constexpr u32 AUDIO_URGENT = 24576;				   // ... and before video if this much is waiting (0.75 s)

constexpr int MAX_CONSECUTIVE_SKIPS = 30;		   // about 1 second of frames with no free buffer
constexpr u32 SLOW_WRITE_MS = 500;
constexpr int MAX_SLOW_WRITES = 6;

struct Slot {
	u8 *base = nullptr;
	u16 *pixels() const { return (u16 *)(base + nvid::VIDEO_FRAME_OFFSET); }
};

Slot slots[SLOT_COUNT];
u8 *capBuf = nullptr;		// camera target of the compact formats (the picture as RGB555; encoded into a slot when a frame is due)
int convSlot = -1;			// slot reserved for the frame waiting for frameFinished()
u32 convTime = 0;
bool haveShown = false;
int qualitySetting = vfmt::DEFAULT_QUALITY;
int recQuality = vfmt::DEFAULT_QUALITY;
u16 recFormat = vfmt::F_RGB555;
u32 slotBytes = nvid::VIDEO_SLOT_BYTES;
u32 convMsMax = 0;
bool highQuality() { return recFormat == vfmt::F_RGB555; }
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
bool audioStartOk = false; // the microphone start succeeded (kept after audioOn is cleared at the end)
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
u32 videoWrites = 0, audioWrites = 0, slow250 = 0, pumpCalls = 0;
u64 bytesWritten = 0;
u32 histogram[reclog::BUCKETS] = {0};
u32 rtcStart = 0;
reclog::RecLog lastLog;

void noteWrite(u32 ms, u32 bytes, bool video) {
	writeMsSum += ms;
	writeCount++;
	if (video) videoWrites++; else audioWrites++;
	bytesWritten += bytes;
	if (ms > writeMsMax)
		writeMsMax = ms;
	if (ms >= 250)
		slow250++;
	histogram[reclog::bucketOf(ms)]++;
}

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
	free(capBuf);
	capBuf = nullptr;
	convSlot = -1;
	haveShown = false;
	freeCount = 0;
	queueHead = queueCount = 0;
	current = shown = -1;
}

bool allocBuffers() {
	recQuality = vfmt::sanitizeQuality(qualitySetting);
	recFormat = vfmt::formatForQuality(recQuality);
	slotBytes = nvid::slotBytesFor(recFormat);
	u32 want = SLOT_BUDGET_BYTES / slotBytes;
	if (want > (u32)SLOT_COUNT)
		want = SLOT_COUNT;
	slotN = 0;
	for (u32 i = 0; i < want; i++) {
		slots[i].base = (u8 *)memalign(32, slotBytes);
		if (!slots[i].base)
			break;
		slotN++;
	}
	if (slotN < MIN_SLOTS) {
		freeBuffers();
		return false;
	}
	if (!highQuality()) {
		capBuf = (u8 *)memalign(32, nvid::FRAME_BYTES);
		if (!capBuf) {
			freeBuffers();
			return false;
		}
	}
	audioChunk = (u8 *)memalign(32, 32 + AUDIO_CHUNK_MAX + nvid::AUDIO_BLOCK_EXTRA);
	if (!audioChunk) {
		freeBuffers();
		return false;
	}
	freeCount = 0;
	const int firstFree = highQuality() ? 1 : 0; // HIGH: slot 0 is the one the camera writes into
	for (int i = slotN - 1; i >= firstFree; i--)
		freeList[freeCount++] = (u8)i;
	current = highQuality() ? 0 : -1;
	shown = -1;
	haveShown = false;
	convSlot = -1;
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
	const bool ok = writer.writeVideoSlot(slots[s].base, t);
	const u32 ms = msclock::toMs(msclock::ticks() - before);
	if (ms > SLOW_WRITE_MS)
		slowWrites++;
	noteWrite(ms, slotBytes, true);
	if (ok)
		storedCount++;

	freeList[freeCount++] = (u8)s;
	if (!ok)
		noteWriteError();
	return ok;
}

bool writeOneAudio(u32 maxBytes) {
	u8 *payload = audioChunk + 16;
	u32 n = audioRec::read(payload, maxBytes & ~15u); // multiples of 16 bytes keep the container aligned
	if (!n)
		return true;
	const u32 before = msclock::ticks();
	const bool ok = writer.writeAudioBlock(audioChunk, n, audioPosBytes / 32);
	noteWrite(msclock::toMs(msclock::ticks() - before), n + 32, false);
	audioPosBytes += n;
	if (!ok)
		noteWriteError();
	return ok;
}

const char *stopReasonName(StopReason r) {
	switch (r) {
		case STOP_USER: return "USER";
		case STOP_SD_FULL: return "SD_FULL";
		case STOP_WRITE_ERROR: return "WRITE_ERROR";
		case STOP_TOO_SLOW: return "TOO_SLOW";
		case STOP_LIMIT: return "LIMIT";
		default: return "NONE";
	}
}

} // namespace

// =================================================================================================
const char *formatName() {
	static char name[24];
	snprintf(name, sizeof name, "NV2-%s-A512", vfmt::formatTag(vfmt::formatForQuality(vfmt::sanitizeQuality(qualitySetting))));
	return name;
}
void setQuality(int q) {
	if (!recording)
		qualitySetting = vfmt::sanitizeQuality(q);
}
int quality() { return recording ? recQuality : qualitySetting; }
u32 maxConvertMs() { return convMsMax; }

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

const reclog::RecLog &lastRecLog() { return lastLog; }

// Writes <videos>/last-recording.txt. Never fails loudly: the log is a diagnostic, not part of the recording.
bool writeLogFile() {
	char text[2600];
	const size_t n = reclog::format(text, sizeof(text), lastLog);
	mkdir((deviceRoot() + "/_nds").c_str(), 0777);
	mkdir((deviceRoot() + "/_nds/nerdMod").c_str(), 0777);
	mkdir(videoFolder().c_str(), 0777);
	FILE *f = fopen((videoFolder() + "/last-recording.txt").c_str(), "wb");
	if (!f)
		return false;
	const bool ok = fwrite(text, 1, n, f) == n;
	return (fclose(f) == 0) && ok;
}

namespace {
void failStart(const char *note, int requested) {
	lastLog = reclog::RecLog();
	reclog::copyStr(lastLog.result, sizeof(lastLog.result), "START_FAILED");
	reclog::copyStr(lastLog.note, sizeof(lastLog.note), note);
	reclog::copyStr(lastLog.stop_reason, sizeof(lastLog.stop_reason), "START_FAILED");
	reclog::copyStr(lastLog.recording_format, sizeof(lastLog.recording_format), formatName());
	lastLog.requested_fps = (u32)requested;
	lastLog.rtc_seconds = 0;
	writeLogFile();
}
} // namespace

bool start(bool innerCamera, std::string &error) {
	if (recording)
		return true;
	if (!sys().fatInitOk() || !ensureVideoFolder()) {
		error = "No SD card";
		failStart(error.c_str(), fps());
		return false;
	}
	if (!allocBuffers()) {
		error = "Out of memory";
		failStart(error.c_str(), fps());
		return false;
	}

	tmpPath = videoFolder() + "/REC_TEMP.nvid.tmp";
	remove(tmpPath.c_str()); // a leftover from an interrupted recording

	audioOn = audioRec::start();
	audioStartOk = audioOn;

	nvid::WriteParams params;
	params.innerCamera = innerCamera;
	params.audio = audioOn;
	params.audioRate = (u16)audioRec::SAMPLE_RATE;
	recFps = fpsutil::sanitize(fpsSetting);
	params.fpsNum = (u16)recFps;
	params.startUnix = (u32)time(NULL);
	params.videoFormat = recFormat;
	convMsMax = 0;
	if (!writer.open(tmpPath, params)) {
		error = (writer.lastError() == ENOSPC) ? "SD card full" : "Cannot create file";
		{
			char note[48];
			snprintf(note, sizeof(note), "%s (errno %d)", error.c_str(), writer.lastError());
			failStart(note, recFps);
		}
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
	videoWrites = audioWrites = slow250 = pumpCalls = 0;
	bytesWritten = 0;
	for (u32 &h : histogram)
		h = 0;
	rtcStart = (u32)time(NULL);
	audioPosBytes = 0;
	msclock::start();
	startTicks = msclock::ticks();
	recording = true;
	return true;
}

u16 *captureTarget() {
	if (!highQuality())
		return (u16 *)capBuf;
	return current >= 0 ? slots[current].pixels() : nullptr;
}
const u16 *lastFrame() {
	if (!highQuality())
		return haveShown ? (const u16 *)capBuf : nullptr;
	return shown >= 0 ? slots[shown].pixels() : nullptr;
}
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
	if (!recording)
		return;
	const bool high = highQuality();
	if (high) {
		if (current < 0)
			return;
		DC_InvalidateRange(slots[current].pixels(), nvid::FRAME_BYTES); // the camera DMA wrote this behind the cache
		shown = current;
	} else {
		DC_InvalidateRange(capBuf, nvid::FRAME_BYTES);
		haveShown = true;
	}

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

	if (high) {
		queueSlot[(queueHead + queueCount) % slotN] = (u8)current;
		queueTime[(queueHead + queueCount) % slotN] = now;
		queueCount++;
		if ((u32)queueCount > bufferPeak)
			bufferPeak = (u32)queueCount;
		current = freeList[--freeCount];
	} else {
		// the picture may still get an effect (frameFinished is called after it): reserve the slot now, encode later
		convSlot = freeList[--freeCount];
		convTime = now;
	}

	u32 k = fpsutil::nearestIndex(now, recFps);
	if (k < nextIndex)
		k = nextIndex;
	nextIndex = k + 1;
}

// Compact formats: encodes the (effect-processed) picture into the reserved slot and queues it. No-op for HIGH.
void frameFinished() {
	if (!recording || highQuality() || convSlot < 0)
		return;
	const u32 before = msclock::ticks();
	u8 *payload = slots[convSlot].base + nvid::VIDEO_FRAME_OFFSET;
	vfmt::encode(recFormat, (const u16 *)capBuf, payload);
	DC_FlushRange(payload, vfmt::frameBytes(recFormat)); // the card reads RAM, not the cache
	const u32 ms = msclock::toMs(msclock::ticks() - before);
	if (ms > convMsMax)
		convMsMax = ms;
	queueSlot[(queueHead + queueCount) % slotN] = (u8)convSlot;
	queueTime[(queueHead + queueCount) % slotN] = convTime;
	queueCount++;
	if ((u32)queueCount > bufferPeak)
		bufferPeak = (u32)queueCount;
	convSlot = -1;
}

void pump() {
	if (!recording || pending != STOP_NONE)
		return;
	pumpCalls++;

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
	fin.audioFailed = !r.hasAudio; // a microphone is always requested
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

	// ---- the persistent log (written for every recording, also for failed or empty ones)
	{
		reclog::RecLog &L = lastLog;
		L = reclog::RecLog();
		L.requested_fps = (u32)recFps;
		L.captured_frames = frames;
		L.duration_ms = endMs;
		L.dropped_frames = fin.droppedFrames;
		L.camera_frames_seen = capturedCount;
		L.buffer_slots = (u32)slotN;
		L.buffer_peak = bufferPeak;
		L.sd_write_count = writeCount;
		L.sd_video_writes = videoWrites;
		L.sd_audio_writes = audioWrites;
		L.sd_bytes = bytesWritten;
		L.sd_total_write_ms = (u32)writeMsSum;
		L.sd_avg_write_ms = r.avgWriteMs;
		L.sd_max_write_ms = writeMsMax;
		L.sd_slow_writes_250ms = slow250;
		L.h0 = histogram[0]; L.h1 = histogram[1]; L.h2 = histogram[2]; L.h3 = histogram[3]; L.h4 = histogram[4]; L.h5 = histogram[5];
		L.aligned_writes = writer.alignedWrites();
		L.loop_iterations = pumpCalls;
		L.rtc_seconds = (u32)time(NULL) - rtcStart;
		{
			char fmtName[24];
			snprintf(fmtName, sizeof fmtName, "NV2-%s-A512", vfmt::formatTag(recFormat));
			reclog::copyStr(L.recording_format, sizeof(L.recording_format), fmtName);
		}
		L.audio_init = (u32)(int32_t)audioRec::startStatus();
		L.audio_callbacks = audioRec::callbacks();
		L.audio_bytes = audioRec::totalDelivered();
		L.audio_samples = audioRec::sampleCount();
		L.audio_chunks = audioWrites;
		L.audio_peak = audioRec::peakSample();
		L.audio_min = audioRec::minSample();
		L.audio_max = audioRec::maxSample();
		L.audio_mean = audioRec::meanSample();
		L.audio_offset_binary = audioRec::wasOffsetBinary() ? 1 : 0;
		L.audio_overrun_bytes = audioRec::overrunBytes();
		L.audio_failed = r.hasAudio ? 0 : 1;
		reclog::copyStr(L.mic_path, sizeof(L.mic_path), audioRec::pathName());
		L.audio_in_rate = audioRec::inputRate();
		L.audio_drains = audioRec::callbacks();
		L.audio_fell_back = audioRec::usedFallback() ? 1 : 0;
		// samples a continuous 16 kHz capture would have delivered in the recording time, and how far the real count is behind (ms)
		L.audio_expected_samples = (u32)(((u64)L.duration_ms * audioRec::sampleRate()) / 1000u);
		L.audio_drift_ms = L.audio_expected_samples > L.audio_samples ? (u32)(((u64)(L.audio_expected_samples - L.audio_samples) * 1000u) / audioRec::sampleRate()) : 0;
		reclog::copyStr(L.mic_class, sizeof(L.mic_class), audiofmt::micClassName(audiofmt::classifyMic(audioStartOk ? audioRec::startStatus() : -1, audioRec::callbacks(), audioRec::peakSample())));
		reclog::copyStr(L.stop_reason, sizeof(L.stop_reason), stopReasonName(r.reason));
		reclog::copyStr(L.result, sizeof(L.result), frames ? "SAVING" : "NOTHING_RECORDED");
	}

	if (frames == 0) {
		remove(tmpPath.c_str()); // nothing worth keeping
		writeLogFile();
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
		reclog::copyStr(lastLog.result, sizeof(lastLog.result), "SAVED");
		reclog::copyStr(lastLog.file, sizeof(lastLog.file), name.c_str());
	} else {
		reclog::copyStr(lastLog.result, sizeof(lastLog.result), "NOT_SAVED");
		// keep the data under its temp name; the user will not see it in the album, but nothing is deleted
		r.saved = false;
	}
	writeLogFile();
	return r;
}

} // namespace rec
