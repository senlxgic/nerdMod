#include <stdio.h>
#include <string.h>
#include <vector>
#include <math.h>
#include "../../arm9/source/audiofmt.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace audiofmt;
int main() {
	// microphone classification
	CHECK(classifyMic(-1, 100, 20000) == MIC_INIT_FAILED);
	CHECK(classifyMic(0, 0, 0) == MIC_NO_CALLBACKS);
	CHECK(classifyMic(0, 50, 0) == MIC_CALLBACKS_BUT_ZERO_DATA);
	CHECK(classifyMic(0, 50, 63) == MIC_CALLBACKS_BUT_ZERO_DATA);
	CHECK(classifyMic(0, 50, 64) == MIC_VALID_DATA);
	CHECK(classifyMic(0, 50, 9000) == MIC_VALID_DATA);
	CHECK(!strcmp(micClassName(MIC_NO_CALLBACKS), "MIC_NO_CALLBACKS"));
	// playback classification
	CHECK(classifyPlayback(false, true, true, 0, 0) == FILE_HAS_NO_AUDIO);
	CHECK(classifyPlayback(true, false, false, 0, 0) == FILE_HAS_AUDIO);
	CHECK(classifyPlayback(true, true, false, 0, 0) == AUDIO_CHANNEL_START_FAILED);
	CHECK(classifyPlayback(true, true, true, 0, 10) == AUDIO_PLAYING);
	CHECK(classifyPlayback(true, true, true, 8, 10) == AUDIO_UNDERRUN);
	CHECK(classifyPlayback(true, true, true, 1, 10) == AUDIO_PLAYING);
	// stats
	std::vector<int16_t> s(160);
	fillTone(s.data(), 160, 1000, 16000, 20000);
	Stats st; accumulate(st, s.data(), 160);
	CHECK(st.samples == 160 && st.peakAbs >= 19900 && st.peakAbs <= 20000);
	CHECK(st.maxV > 0 && st.minV < 0);
	CHECK(mean(st) > -50 && mean(st) < 50);          // a sine has no DC
	// the tone: 1 kHz at 16 kHz has a 16-sample period and the right amplitude
	CHECK(s[0] == 0 && s[4] == 20000 && s[12] == -20000);
	std::vector<int16_t> d(160, 2048);               // constant DC (unsigned-12-bit style data)
	Stats dc; accumulate(dc, d.data(), 160);
	CHECK(mean(dc) == 2048 && dc.peakAbs == 2048);
	printf(failures ? "%d FAILURES\n" : "audio class tests OK\n", failures);
	return failures;
}
