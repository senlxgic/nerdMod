#include "audioRecorder.h"

#include <malloc.h>
#include <string.h>

#include "audiofmt.h"
#include "audioresample.h"
#include "camera.h"
#include "camera_protocol.h"
#include "msclock.h"

namespace audioRec {

namespace {

constexpr u32 MIC_BUFFER_BYTES = 16384;		   // libnds ARM7 <-> ARM9 buffer (0.5 s); 32-byte multiple
constexpr u32 RING_BYTES = 128 * 1024;		   // 4 s of 16 kHz audio between the capture and the SD writer
constexpr u32 NDMA_LINES = NMMIC_RING_BYTES / 32;
constexpr u32 NO_DATA_FALLBACK_MS = 1200;

enum Path { P_NONE = 0, P_LIBNDS, P_NDMA };

u8 *micBuffer = nullptr; // libnds path
u8 *hwRing = nullptr;	 // NDMA path: the ring the ARM7's NDMA channel fills
u8 *ring = nullptr;		 // 16 kHz PCM16 mono, what the recorder reads
volatile u32 ringHead = 0;
volatile u32 ringTail = 0;
volatile u32 delivered = 0;
volatile u32 overrun = 0;
volatile bool anyData = false;
volatile bool active = false;
volatile bool offsetBinary = false;
volatile u32 callbackCount = 0;
audiofmt::Stats stats;
int startResult = 0;

Path path = P_NONE;
const char *lastPath = "none";
bool fellBack = false;
u32 hwTail = 0;			// next line (32 bytes) of hwRing to consume
u32 inRate = 0;
u32 drainCount = 0;
u32 inSamples = 0;
u32 startTicks = 0;
audioresample::Linear resampler;

// Appends `count` PCM16 samples to the 16 kHz ring (drops the whole block if the writer is too far behind).
void storeBlock(const int16_t *src, u32 count, u16 flip) {
	const u32 bytes = count * 2;
	const u32 used = ringHead - ringTail;
	if (used + bytes > RING_BYTES) {
		overrun += bytes;
		return;
	}
	int16_t *dst = (int16_t *)ring;
	u32 idx = (ringHead % RING_BYTES) / 2;
	const u32 ringSamples = RING_BYTES / 2;
	int32_t mn = stats.minV, mx = stats.maxV;
	int64_t sum = 0;
	uint32_t pk = stats.peakAbs;
	for (u32 i = 0; i < count; i++) {
		const int16_t v = (int16_t)((u16)src[i] ^ flip);
		dst[idx] = v;
		if (++idx == ringSamples)
			idx = 0;
		if (v < mn) mn = v;
		if (v > mx) mx = v;
		sum += v;
		const uint32_t a = (uint32_t)(v < 0 ? -(int32_t)v : (int32_t)v);
		if (a > pk) pk = a;
	}
	stats.minV = mn;
	stats.maxV = mx;
	stats.sum += sum;
	stats.peakAbs = pk;
	stats.samples += count;
	delivered += bytes;
	ringHead += bytes;
}

// libnds path: runs when the ARM7 has filled (part of) the microphone buffer. The shared buffer is only READ here.
void micCallback(void *completedBuffer, int length) {
	if (!active || !ring || length <= 0)
		return;
	const u32 bytes = (u32)length & ~1u;
	DC_InvalidateRange(completedBuffer, (u32)length);
	callbackCount++;
	anyData = true;
	const int16_t *src = (const int16_t *)completedBuffer;
	const u32 count = bytes / 2;
	if (!offsetBinary && audiofmt::looksOffsetBinary(src, count))
		offsetBinary = true;
	storeBlock(src, count, offsetBinary ? 0x8000u : 0u);
}

bool startLibnds() {
	if (!micBuffer)
		micBuffer = (u8 *)memalign(32, MIC_BUFFER_BYTES);
	if (!micBuffer)
		return false;
	memset(micBuffer, 0, MIC_BUFFER_BYTES);
	DC_FlushRange(micBuffer, MIC_BUFFER_BYTES);
	// libnds' ARM7 picks the DSi codec (TWL) or the classic SPI microphone itself; 12-bit samples are shifted up to PCM16
	startResult = soundMicRecord(micBuffer, MIC_BUFFER_BYTES, MicFormat_12Bit, SAMPLE_RATE, micCallback);
	if (startResult < 0)
		return false;
	path = P_LIBNDS;
	lastPath = fellBack ? "libnds (fallback)" : "libnds";
	return true;
}

// NDMA path: ask the camera ARM7 to run the microphone FIFO through an NDMA channel into hwRing.
bool startNdma() {
	if (!cameraHardwareAccessible())
		return false;
	if (!hwRing)
		hwRing = (u8 *)memalign(32, NMMIC_RING_BYTES);
	if (!hwRing)
		return false;
	u32 *w = (u32 *)hwRing;
	for (u32 i = 0; i < NMMIC_RING_BYTES / 4; i++)
		w[i] = NMMIC_SENTINEL;
	DC_FlushRange(hwRing, NMMIC_RING_BYTES);
	hwTail = 0;
	u32 reply = 0;
	const u32 addr = (u32)(uintptr_t)hwRing;
	if (!cameraRawCommand(NMCAM_CMD_MIC_START_BASE | (addr >> 5), 90, &reply))
		return false;
	inRate = (reply & 1u) ? 23802u : 16364u; // codec rate 47.6 / 32.7 kHz divided by two
	resampler.init(inRate, SAMPLE_RATE);
	startResult = 17; // "channel", for the log: not a libnds channel but non-negative like a success
	path = P_NDMA;
	lastPath = "ndma";
	return true;
}

void fallBackToLibnds() {
	cameraRawCommand(NMCAM_CMD_MIC_STOP, 30, nullptr);
	fellBack = true;
	path = P_NONE;
	if (!startLibnds())
		active = false;
}

// Moves every completed 32-byte line of the NDMA ring into the 16 kHz ring (converting the rate on the way).
void drainNdma() {
	if (path != P_NDMA || !ring || !hwRing)
		return;
	u32 lines = 0;
	while (lines < NDMA_LINES / 2) {
		const u32 cur = hwTail, next = (cur + 1) % NDMA_LINES;
		u32 *nl = (u32 *)(hwRing + next * 32);
		DC_InvalidateRange(nl, 32);
		if (nl[0] == NMMIC_SENTINEL)
			break; // the NDMA has not got past line `cur` yet
		u32 *cl = (u32 *)(hwRing + cur * 32);
		DC_InvalidateRange(cl, 32);
		int16_t tmp[40];
		u32 n = 0;
		const int16_t *s = (const int16_t *)cl;
		for (u32 i = 0; i < 16; i++)
			resampler.push(s[i], [&](int16_t v) {
				if (n < 40)
					tmp[n++] = v;
			});
		storeBlock(tmp, n, 0);
		inSamples += 16;
		for (u32 i = 0; i < 8; i++)
			cl[i] = NMMIC_SENTINEL;
		DC_FlushRange(cl, 32);
		hwTail = next;
		lines++;
	}
	if (lines) {
		drainCount++;
		anyData = true;
	} else if (!anyData && msclock::toMs(msclock::ticks() - startTicks) > NO_DATA_FALLBACK_MS) {
		fallBackToLibnds(); // the hardware path produced nothing: use the libnds service instead
	}
}

} // namespace

bool start() {
	if (active)
		return true;
	if (!ring)
		ring = (u8 *)malloc(RING_BYTES);
	if (!ring) {
		release();
		return false;
	}
	msclock::ensureStarted();
	ringHead = ringTail = 0;
	delivered = overrun = 0;
	anyData = false;
	offsetBinary = false;
	callbackCount = 0;
	stats = audiofmt::Stats();
	startResult = 0;
	fellBack = false;
	drainCount = 0;
	inSamples = 0;
	inRate = 0;
	path = P_NONE;
	active = true;
	startTicks = msclock::ticks();
	if (startNdma())
		return true;
	if (startLibnds())
		return true;
	active = false;
	path = P_NONE;
	return false;
}

void stop() {
	if (!active)
		return;
	if (path == P_NDMA) {
		drainNdma();
		cameraRawCommand(NMCAM_CMD_MIC_STOP, 30, nullptr);
	} else if (path == P_LIBNDS) {
		soundMicOff();
	}
	active = false;
	path = P_NONE;
}

void release() {
	stop();
	free(micBuffer);
	free(ring);
	free(hwRing);
	micBuffer = nullptr;
	ring = nullptr;
	hwRing = nullptr;
}

bool running() { return active; }
bool gotData() {
	drainNdma();
	return anyData;
}
u32 totalDelivered() { return delivered; }
u32 peakSample() { return stats.peakAbs; }
u32 callbacks() { return lastPath[0] == 'n' ? drainCount : callbackCount; }
u32 sampleCount() { return stats.samples; }
int minSample() { return stats.samples ? stats.minV : 0; }
int maxSample() { return stats.samples ? stats.maxV : 0; }
int meanSample() { return audiofmt::mean(stats); }
bool wasOffsetBinary() { return offsetBinary; }
int startStatus() { return startResult; }
u32 overrunBytes() { return overrun; }
u32 sampleRate() { return SAMPLE_RATE; }
const char *pathName() { return lastPath; }
u32 inputRate() { return inRate; }
bool usedFallback() { return fellBack; }

u32 available() {
	drainNdma();
	return (ringHead - ringTail) & ~1u;
}

u32 read(u8 *dst, u32 maxBytes) {
	if (!ring)
		return 0;
	u32 n = available();
	if (n > maxBytes)
		n = maxBytes;
	n &= ~1u;
	const u32 pos = ringTail % RING_BYTES;
	const u32 first = (pos + n <= RING_BYTES) ? n : RING_BYTES - pos;
	memcpy(dst, ring + pos, first);
	if (first < n)
		memcpy(dst + first, ring, n - first);
	ringTail += n;
	return n;
}

} // namespace audioRec
