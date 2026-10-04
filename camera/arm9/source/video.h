/*
	Video recorder for nerdMod Camera.

	The recorder owns a small ring of frame buffers in main RAM. While recording, the camera DMA writes straight into the
	next free buffer (instead of VRAM); the finished frame is copied to the screen by the caller and, when a frame is due
	(10 per second), put on a queue. pump() writes the queue to the SD card, one large sequential write per call, so the
	loop that also feeds the camera is never blocked for more than one write. When the card cannot keep up, the queue
	fills, due frames are skipped (and counted) and the recording continues; it is stopped cleanly if that persists.

	The file is written as REC_TEMP.nvid.tmp and renamed to NV_YYYYMMDD_HHMMSS.nvid after a clean stop. A leftover
	temp file (power loss) is never listed and is overwritten by the next recording.

	No statvfs() anywhere: a full card shows up as ENOSPC from a write.
*/
#pragma once

#include <nds.h>

#include <string>

namespace rec {

constexpr u32 FRAME_INTERVAL_MS = 100;			// 10 frames per second
constexpr u32 MAX_DURATION_MS = 30u * 60u * 1000u;
constexpr u32 MAX_FILE_BYTES = 1500u * 1024u * 1024u;

enum StopReason {
	STOP_USER = 0,
	STOP_SD_FULL,	  // the card is full
	STOP_WRITE_ERROR, // any other write error
	STOP_TOO_SLOW,	  // the card could not keep up for too long
	STOP_LIMIT,		  // maximum duration / file size reached
	STOP_NONE
};

struct Result {
	bool saved = false;		// a playable file exists
	std::string name;		// file name inside the video folder
	u32 frames = 0;
	u32 droppedFrames = 0;
	u32 durationMs = 0;
	bool hasAudio = false;	// the microphone delivered data
	StopReason reason = STOP_USER;
};

std::string videoFolder();
bool ensureVideoFolder();

// Tells whether the recorder could be started right now (memory, folder, microphone state are checked in start()).
bool start(bool innerCamera, std::string &error);
bool active();

// Where the next camera DMA must write (a 32-byte aligned buffer of nvid::FRAME_BYTES).
u16 *captureTarget();
// Call when that DMA has finished. The frame is then available through lastFrame() until the next call.
void frameCaptured();
const u16 *lastFrame();

// Writes at most one chunk to the card. Call once per main-loop iteration.
void pump();

// Seconds / milliseconds since the recording started.
u32 elapsedMs();

// Set once the recorder wants to stop by itself; stop() must then be called.
StopReason autoStopReason();

// Stops the microphone, writes what is queued, finalises and renames the file.
Result stop(StopReason reason);

// Statistics for the on-screen warning ("SD slow").
u32 queuedFrames();
u32 skippedDueFrames();
// True once the microphone has delivered samples (false after a while = no sound is being recorded).
bool hasMicData();

} // namespace rec
