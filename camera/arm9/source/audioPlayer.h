/*
	Audio for the NERDVID player: PCM16 mono 16 kHz streamed through one looping hardware sound channel.

	The ring is written ahead of the playback position and silenced behind it, so an SD read that comes late plays
	silence, never stale audio. Playback is never restarted to chase the video; the player skips late video frames
	instead. Seeking and resuming stop the channel and start it again at the new position.
*/
#pragma once

#include <nds.h>

namespace audioPlay {

constexpr u32 RATE = 16000;

// Starts silent playback at media time startMs. Returns false when there is no memory or no sound channel.
bool start(u32 startMs);
// Adds a chunk read from the file (timeMs = media time of its first sample).
void feed(u32 timeMs, const u8 *pcm, u32 bytes);
// Call once per frame: silences what has been played.
void update();
// Silences and releases the channel. Safe to call twice.
void stop();
bool active();
// Media position in ms according to the audio clock (0 when not active).
u32 positionMs();
// Chunks that arrived too late to be played.
u32 lateChunks();

} // namespace audioPlay
