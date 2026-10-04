#include "audioRecorder.h"

#include <malloc.h>
#include <string.h>

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

// Runs when the ARM7 has filled (part of) the microphone buffer. Keep it short: invalidate, copy, bump an index.
void micCallback(void *completedBuffer, int length) {
	if (!active || !ring || length <= 0)
		return;
	u32 bytes = (u32)length & ~1u;
	DC_InvalidateRange(completedBuffer, (u32)length);
	delivered += bytes;
	anyData = true;

	const u32 used = ringHead - ringTail;
	if (used + bytes > RING_BYTES) {
		overrun += bytes; // the writer is too far behind: drop this block rather than overwrite unread data
		return;
	}
	const u32 pos = ringHead % RING_BYTES;
	const u32 first = (pos + bytes <= RING_BYTES) ? bytes : RING_BYTES - pos;
	memcpy(ring + pos, completedBuffer, first);
	if (first < bytes)
		memcpy(ring, (const u8 *)completedBuffer + first, bytes - first);
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
	active = true;
	// 12-bit samples come back shifted up to 16-bit PCM
	soundMicRecord(micBuffer, MIC_BUFFER_BYTES, MicFormat_12Bit, SAMPLE_RATE, micCallback);
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
