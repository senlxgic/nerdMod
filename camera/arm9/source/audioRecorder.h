/*
	Microphone capture for the video recorder.

	The ARM7 (libnds' sound FIFO service) samples the microphone into a small 32-byte aligned buffer owned by the ARM9
	and reports every completed part through a callback. The callback only copies those bytes into a larger ring
	(a few seconds of audio), so the SD writer in the main loop can fall behind for a while without losing sound.
	No per-sample work happens on the ARM9 and nothing here touches the camera ARM7 I2C traffic.

	16 kHz, signed 16-bit, mono = 32 KB/s.
*/
#pragma once

#include <nds.h>

namespace audioRec {

constexpr u32 SAMPLE_RATE = 16000;

// Starts the microphone. Returns false if the ring could not be allocated or the ARM7 refused the request.
bool start();
// Stops the microphone and releases the buffers once the ring has been read out (call available()/read() first).
void stop();
void release();

bool running();
// Bytes waiting in the ring (always a multiple of 2).
u32 available();
// Copies up to maxBytes (rounded down to a multiple of 2) out of the ring.
u32 read(u8 *dst, u32 maxBytes);
// Total bytes the microphone has delivered so far (including bytes that did not fit into the ring).
u32 totalDelivered();
// Bytes that were lost because the ring was full.
u32 overrunBytes();
// True once the ARM7 has delivered at least one buffer; false after a while means "no microphone data".
bool gotData();

// Loudest sample seen so far (0..32768), whether the data had to be converted from offset binary, and the value
// soundMicRecord() returned. Shown on the camera diagnostics page.
u32 peakSample();
bool wasOffsetBinary();
int startStatus();
// Phase 2C.1 telemetry: number of callbacks, samples stored, and their range / mean after the sign fix.
u32 callbacks();
u32 sampleCount();
int minSample();
int maxSample();
int meanSample();

// Phase 2D: the sample rate the capture actually runs at, and which capture path is active ("libnds", "ndma", "none").
u32 sampleRate();
const char *pathName();

} // namespace audioRec
