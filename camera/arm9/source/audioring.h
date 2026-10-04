/*
	Placement of audio chunks in the playback ring (pure maths, host-tested).

	Audio is addressed in "stream bytes": media time in ms * 32 (16 kHz, PCM16, mono). The ring plays from stream
	position s0 (where playback started); a stream byte b lives at ring index (b - s0) % ring.
*/
#pragma once

#include <stdint.h>

namespace audioring {

constexpr uint32_t BYTES_PER_MS = 32;

struct Placement {
	bool drop = true;		// nothing of this chunk can be used
	uint32_t skip = 0;		// bytes to cut from the front of the chunk (already played / before the start)
	uint32_t count = 0;		// bytes to write
	uint32_t ringIndex = 0; // where the first written byte goes
};

// s: stream position of the chunk, n: its size, consumed: stream position being played right now,
// margin: bytes ahead of `consumed` that are already in flight and must not be touched.
inline Placement place(uint32_t s0, uint32_t consumed, uint32_t s, uint32_t n, uint32_t ringBytes, uint32_t margin) {
	Placement p;
	n &= ~1u;
	s &= ~1u;
	if (!n)
		return p;
	uint32_t from = s;
	uint32_t end = s + n;
	const uint32_t floorPos = (consumed + margin > s0) ? consumed + margin : s0;
	if (end <= floorPos)
		return p; // too late
	if (from < floorPos)
		from = (floorPos + 1) & ~1u;
	if (end <= from)
		return p;
	// the ring holds [floorPos, floorPos + ringBytes - margin)
	const uint32_t limit = floorPos + ringBytes - margin;
	if (from >= limit)
		return p; // too far ahead
	if (end > limit)
		end = limit & ~1u;
	if (end <= from)
		return p;
	p.drop = false;
	p.skip = from - s;
	p.count = end - from;
	p.ringIndex = (from - s0) % ringBytes;
	return p;
}

} // namespace audioring
