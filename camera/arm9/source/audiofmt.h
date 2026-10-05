// Sample-format helpers for the microphone path (pure, host-tested).
#pragma once

#include <stdint.h>

namespace audiofmt {

// libnds delivers 12-bit microphone samples shifted up to 16 bit. Depending on the path (NTR SPI or TWL codec) they
// may be centred on 0 (signed) or on 0x8000 (offset binary). Read as signed, an offset-binary block has a large
// mean magnitude (every sample sits near +-32768) while a real signal sits near 0. Silence (all zero) is signed.
inline bool looksOffsetBinary(const int16_t *s, uint32_t n) {
	if (!n)
		return false;
	uint64_t sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum += (uint64_t)(s[i] < 0 ? -(int32_t)s[i] : (int32_t)s[i]);
	return sum / n > 20000u;
}

inline void flipToSigned(int16_t *s, uint32_t n) {
	uint16_t *u = (uint16_t *)s;
	for (uint32_t i = 0; i < n; i++)
		u[i] ^= 0x8000u;
}

// Peak magnitude of a block (0..32768).
inline uint32_t peak(const int16_t *s, uint32_t n) {
	uint32_t p = 0;
	for (uint32_t i = 0; i < n; i++) {
		const int32_t v = s[i];
		const uint32_t a = (uint32_t)(v < 0 ? -v : v);
		if (a > p)
			p = a;
	}
	return p;
}

// Running statistics of the samples delivered by the microphone (after the sign fix).
struct Stats {
	uint32_t samples = 0;
	int32_t minV = 32767, maxV = -32768;
	int64_t sum = 0;
	uint32_t peakAbs = 0;
};

inline void accumulate(Stats &st, const int16_t *s, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		const int32_t v = s[i];
		if (v < st.minV) st.minV = v;
		if (v > st.maxV) st.maxV = v;
		st.sum += v;
		const uint32_t a = (uint32_t)(v < 0 ? -v : v);
		if (a > st.peakAbs) st.peakAbs = a;
	}
	st.samples += n;
}

inline int32_t mean(const Stats &st) { return st.samples ? (int32_t)(st.sum / (int64_t)st.samples) : 0; }

// Microphone result, from what was observed (Phase 2C.1). "Initialised" is not "recorded sound".
enum MicClass { MIC_INIT_FAILED = 0, MIC_NO_CALLBACKS, MIC_CALLBACKS_BUT_ZERO_DATA, MIC_VALID_DATA };

constexpr uint32_t VALID_PEAK = 64; // 12-bit noise floor shifted up by 4 is above this on a live microphone

inline MicClass classifyMic(int initResult, uint32_t callbacks, uint32_t peakAbs) {
	if (initResult < 0)
		return MIC_INIT_FAILED;
	if (callbacks == 0)
		return MIC_NO_CALLBACKS;
	if (peakAbs < VALID_PEAK)
		return MIC_CALLBACKS_BUT_ZERO_DATA;
	return MIC_VALID_DATA;
}

inline const char *micClassName(MicClass c) {
	switch (c) {
		case MIC_INIT_FAILED: return "MIC_INIT_FAILED";
		case MIC_NO_CALLBACKS: return "MIC_NO_CALLBACKS";
		case MIC_CALLBACKS_BUT_ZERO_DATA: return "MIC_CALLBACKS_BUT_ZERO_DATA";
		default: return "MIC_VALID_DATA";
	}
}

// Playback side (the player), distinct from the recording side.
enum PlayClass { FILE_HAS_NO_AUDIO = 0, FILE_HAS_AUDIO, AUDIO_CHANNEL_START_FAILED, AUDIO_UNDERRUN, AUDIO_PLAYING };

inline PlayClass classifyPlayback(bool fileHasAudio, bool startAttempted, bool startOk, uint32_t lateChunks, uint32_t chunksFed) {
	if (!fileHasAudio)
		return FILE_HAS_NO_AUDIO;
	if (!startAttempted)
		return FILE_HAS_AUDIO;
	if (!startOk)
		return AUDIO_CHANNEL_START_FAILED;
	if (lateChunks > 0 && lateChunks * 2 >= chunksFed + 1)
		return AUDIO_UNDERRUN;
	return AUDIO_PLAYING;
}

inline const char *playClassName(PlayClass c) {
	switch (c) {
		case FILE_HAS_NO_AUDIO: return "FILE_HAS_NO_AUDIO";
		case FILE_HAS_AUDIO: return "FILE_HAS_AUDIO";
		case AUDIO_CHANNEL_START_FAILED: return "AUDIO_CHANNEL_START_FAILED";
		case AUDIO_UNDERRUN: return "AUDIO_UNDERRUN";
		default: return "AUDIO_PLAYING";
	}
}

// Fills `out` with a sine wave (signed PCM16). Used by the player self-test; freq in Hz, rate in Hz, amp 0..32767.
inline void fillTone(int16_t *out, uint32_t n, uint32_t freq, uint32_t rate, int32_t amp) {
	// integer triangle-free sine via a 64-entry table would do; a 16 kHz / 1 kHz tone has a 16-sample period
	static const int16_t tbl[16] = {0, 12540, 23170, 30274, 32767, 30274, 23170, 12540, 0, -12540, -23170, -30274, -32767, -30274, -23170, -12540};
	uint32_t phase = 0; // 16.16 fixed point index into the 16-entry table
	const uint32_t step = (uint32_t)(((uint64_t)freq * 16u << 16) / rate);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = (int16_t)(((int64_t)tbl[(phase >> 16) & 15] * amp) / 32767);
		phase += step;
	}
}

} // namespace audiofmt
