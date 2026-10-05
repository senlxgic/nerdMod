#include "audioRecorder.h"

#include <malloc.h>
#include <string.h>

#include "audiofmt.h"

namespace audioRec {

namespace {

constexpr u32 MIC_BUFFER_BYTES = 16384;		   // ARM7 <-> ARM9 buffer (0.5 s); 32-byte multiple
constexpr u32 RING_BYTES = 128 * 1024;		   // 4 s of audio between the callback and the SD writer

u8 *micBuffer = nullptr;
u8 *ring = nullptr;
volatile u32 ringHead = 0;		// total bytes ever written (callback)
volatile u32 ringTail = 0;		// total bytes ever read (main loop)
volatile u32 delivered = 0;
volatile u32 overrun = 0;
volatile bool anyData = false;
volatile bool active = false;
volatile bool offsetBinary = false;
volatile u32 callbackCount = 0;
audiofmt::Stats stats;
int startResult = 0;

// Runs when the ARM7 has filled (part of) the microphone buffer. Keep it short: invalidate, convert while copying, bump an index.
// The shared buffer is only READ here: writing into it from the ARM9 would leave dirty cache lines that could later overwrite
// fresh samples written by the ARM7.
void micCallback(void *completedBuffer, int length) {
	if (!active || !ring || length <= 0)
		return;
	u32 bytes = (u32)length & ~1u;
	DC_InvalidateRange(completedBuffer, (u32)length);
	callbackCount++;
	delivered += bytes;
	anyData = true;

	const int16_t *src = (const int16_t *)completedBuffer;
	const u32 count = bytes / 2;
	// libnds delivers the sample either signed or offset binary depending on the path (see audiofmt.h)
	if (!offsetBinary && audiofmt::looksOffsetBinary(src, count))
		offsetBinary = true;

	const u32 used = ringHead - ringTail;
	if (used + bytes > RING_BYTES) {
		overrun += bytes; // the writer is too far behind: drop this block rather than overwrite unread data
		return;
	}
	int16_t *dst = (int16_t *)ring;
	u32 idx = (ringHead % RING_BYTES) / 2;
	const u32 ringSamples = RING_BYTES / 2;
	const u16 flip = offsetBinary ? 0x8000u : 0u;
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
	ringHead += bytes;
}

} // namespace

bool start() {
	if (active)
		return true;
	if (!micBuffer)
		micBuffer = (u8 *)memalign(32, MIC_BUFFER_BYTES);
	if (!ring)
		ring = (u8 *)malloc(RING_BYTES);
	if (!micBuffer || !ring) {
		release();
		return false;
	}
	memset(micBuffer, 0, MIC_BUFFER_BYTES);
	DC_FlushRange(micBuffer, MIC_BUFFER_BYTES);
	ringHead = ringTail = 0;
	delivered = overrun = 0;
	anyData = false;
	offsetBinary = false;
	callbackCount = 0;
	stats = audiofmt::Stats();
	startResult = 0;
	active = true;
	// libnds' ARM7 picks the DSi codec (TWL) or the classic SPI microphone itself; 12-bit samples are shifted up to PCM16
	startResult = soundMicRecord(micBuffer, MIC_BUFFER_BYTES, MicFormat_12Bit, SAMPLE_RATE, micCallback);
	if (startResult < 0) {
		active = false;
		return false;
	}
	return true;
}

void stop() {
	if (!active)
		return;
	soundMicOff();
	active = false;
}

void release() {
	stop();
	free(micBuffer);
	free(ring);
	micBuffer = nullptr;
	ring = nullptr;
}

bool running() { return active; }
bool gotData() { return anyData; }
u32 totalDelivered() { return delivered; }
u32 peakSample() { return stats.peakAbs; }
u32 callbacks() { return callbackCount; }
u32 sampleCount() { return stats.samples; }
int minSample() { return stats.samples ? stats.minV : 0; }
int maxSample() { return stats.samples ? stats.maxV : 0; }
int meanSample() { return audiofmt::mean(stats); }
bool wasOffsetBinary() { return offsetBinary; }
u32 sampleRate() { return SAMPLE_RATE; }
const char *pathName() { return active ? "libnds" : "none"; }
int startStatus() { return startResult; }
u32 overrunBytes() { return overrun; }

u32 available() { return (ringHead - ringTail) & ~1u; }

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
