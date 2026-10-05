/*
	nerdMod Music: streaming playback.

	file -> decoder (minimp3 or PCM WAV) -> two mono 16-bit ring buffers (left / right hardware channels, looping)

	Nothing is ever loaded whole: an MP3 is read through a 16 KiB window, a WAV through 2 KiB reads. The output side uses
	the same primitives as the video player's audio (soundEnable, a looping hardware channel, DC_FlushRange, the msclock
	clock) but runs two channels for stereo and keeps the clock locked to the real sound-timer period.
*/
#pragma once

#include <nds.h>

#include "common/nmmusic.h"

namespace mplay {

enum class State : u8 { Idle, Buffering, Playing, Paused, Finished, Error };

// Opens the track and starts buffering at startMs (0 = beginning). On failure returns false and lastError() says why.
bool open(const nmmusic::Track &t, u32 startMs = 0);
// Call every frame: refills the rings and tracks the state.
void update();
void pause();
void resume();
void togglePause();
void seekPermille(u32 permille);
void seekRelativeMs(int deltaMs);
void stop();

void setVolume(int v); // 0..127
int volume();

State state();
u32 positionMs();
u32 durationMs();
u32 sampleRate();
u32 underruns();
// Peak of the most recently decoded block, 0..32767 (left, right), for the level meter.
void levels(int &l, int &r);
const char *lastError();
const char *stateName();

} // namespace mplay
