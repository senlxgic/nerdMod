#include "audioPlayer.h"

#include <malloc.h>
#include <string.h>

#include "audioring.h"
#include "msclock.h"

namespace audioPlay {

namespace {

constexpr u32 RING_BYTES = 128 * 1024;	 // 4 s
constexpr u32 MARGIN = 1024;			 // 32 ms already queued in the hardware
constexpr u32 SCRUB_BEHIND = 2048;

u8 *ring = nullptr;
int channel = -1;
u32 s0 = 0;			 // stream position at the start
u32 startTicks = 0;
u32 scrubbed = 0;	 // stream position up to which the ring has been silenced
u32 late = 0;

u32 consumed() { return s0 + msclock::toMs(msclock::ticks() - startTicks) * audioring::BYTES_PER_MS; }

} // namespace

bool start(u32 startMs) {
	stop();
	if (!ring)
		ring = (u8 *)memalign(32, RING_BYTES);
	if (!ring)
		return false;
	memset(ring, 0, RING_BYTES);
	DC_FlushRange(ring, RING_BYTES);
	s0 = startMs * audioring::BYTES_PER_MS;
	scrubbed = s0;
	late = 0;
	startTicks = msclock::ticks();
	channel = soundPlaySample(ring, SoundFormat_16Bit, RING_BYTES, RATE, 127, 64, true, 0);
	if (channel < 0) {
		free(ring);
		ring = nullptr;
		return false;
	}
	return true;
}

void feed(u32 timeMs, const u8 *pcm, u32 bytes) {
	if (channel < 0 || !ring)
		return;
	const audioring::Placement p = audioring::place(s0, consumed(), timeMs * audioring::BYTES_PER_MS, bytes, RING_BYTES, MARGIN);
	if (p.drop) {
		late++;
		return;
	}
	const u32 first = (p.ringIndex + p.count <= RING_BYTES) ? p.count : RING_BYTES - p.ringIndex;
	memcpy(ring + p.ringIndex, pcm + p.skip, first);
	DC_FlushRange(ring + p.ringIndex, first);
	if (first < p.count) {
		memcpy(ring, pcm + p.skip + first, p.count - first);
		DC_FlushRange(ring, p.count - first);
	}
}

void update() {
	if (channel < 0 || !ring)
		return;
	const u32 now = consumed();
	if (now < s0 + SCRUB_BEHIND)
		return;
	const u32 upTo = (now - SCRUB_BEHIND) & ~31u;
	if (upTo <= scrubbed)
		return;
	u32 n = upTo - scrubbed;
	if (n > RING_BYTES)
		n = RING_BYTES;
	u32 idx = (scrubbed - s0) % RING_BYTES;
	while (n) {
		const u32 run = (idx + n <= RING_BYTES) ? n : RING_BYTES - idx;
		memset(ring + idx, 0, run);
		DC_FlushRange(ring + idx, run);
		idx = (idx + run) % RING_BYTES;
		n -= run;
	}
	scrubbed = upTo;
}

void stop() {
	if (channel >= 0) {
		soundKill(channel);
		channel = -1;
	}
	free(ring);
	ring = nullptr;
}

bool active() { return channel >= 0; }
u32 positionMs() { return channel >= 0 ? (consumed() / audioring::BYTES_PER_MS) : 0; }
u32 lateChunks() { return late; }

} // namespace audioPlay
