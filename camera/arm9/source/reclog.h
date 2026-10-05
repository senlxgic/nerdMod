/*
	The persistent recording log (Phase 2C.1): <sd>:/_nds/nerdMod/videos/last-recording.txt, rewritten after every
	recording attempt, successful or not. Pure text formatting and parsing, so the host tests can exercise it.
*/
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace reclog {

// "fps x100" / "MB/s x100" helpers (integer maths, no floating point on the DS)
inline uint32_t fpsX100(uint32_t frames, uint32_t durationMs) {
	return durationMs ? (uint32_t)(((uint64_t)frames * 100000u + durationMs / 2u) / durationMs) : 0;
}
// Decimal megabytes per second (x100) from bytes and the time spent inside write().
inline uint32_t mbPerSecX100(uint64_t bytes, uint32_t writeMs) {
	return writeMs ? (uint32_t)((bytes * 100u) / ((uint64_t)writeMs * 1000u)) : 0;
}

// Write-latency histogram buckets (ms): <20, <50, <100, <250, <500, >=500
constexpr int BUCKETS = 6;
inline int bucketOf(uint32_t ms) {
	if (ms < 20) return 0;
	if (ms < 50) return 1;
	if (ms < 100) return 2;
	if (ms < 250) return 3;
	if (ms < 500) return 4;
	return 5;
}

#define RECLOG_U32_FIELDS(X) \
	X(requested_fps) X(captured_frames) X(duration_ms) X(dropped_frames) X(camera_frames_seen) X(buffer_slots) \
	X(buffer_peak) X(sd_write_count) X(sd_video_writes) X(sd_audio_writes) X(sd_total_write_ms) X(sd_avg_write_ms) \
	X(sd_max_write_ms) X(sd_slow_writes_250ms) X(loop_iterations) X(rtc_seconds) X(aligned_writes) \
	X(audio_init) X(audio_callbacks) X(audio_bytes) X(audio_samples) X(audio_chunks) X(audio_peak) X(audio_failed) \
	X(audio_offset_binary) X(audio_overrun_bytes) X(h0) X(h1) X(h2) X(h3) X(h4) X(h5)

struct RecLog {
#define X(n) uint32_t n = 0;
	RECLOG_U32_FIELDS(X)
#undef X
	uint64_t sd_bytes = 0;
	int32_t audio_min = 0, audio_max = 0, audio_mean = 0;
	char recording_format[24] = "";
	char mic_class[32] = "";
	char stop_reason[24] = "";
	char file[40] = "";
	char result[24] = "";	// SAVED / NOT_SAVED / START_FAILED
	char note[64] = "";
};

inline size_t format(char *out, size_t cap, const RecLog &r) {
	const uint32_t afps = fpsX100(r.captured_frames, r.duration_ms);
	const uint32_t mbs = mbPerSecX100(r.sd_bytes, r.sd_total_write_ms);
	size_t n = 0;
	auto put = [&](const char *fmt, auto... a) {
		if (n + 1 >= cap)
			return;
		const int w = snprintf(out + n, cap - n, fmt, a...);
		if (w > 0)
			n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
	};
	put("%s", "nerdMod last recording\n");
	put("result=%s\n", r.result);
	put("file=%s\n", r.file);
	put("stop_reason=%s\n", r.stop_reason);
	put("note=%s\n", r.note);
	put("requested_fps=%u\n", (unsigned)r.requested_fps);
	put("captured_frames=%u\n", (unsigned)r.captured_frames);
	put("duration_ms=%u\n", (unsigned)r.duration_ms);
	put("actual_fps=%u.%02u\n", (unsigned)(afps / 100), (unsigned)(afps % 100));
	put("dropped_frames=%u\n", (unsigned)r.dropped_frames);
	put("camera_frames_seen=%u\n", (unsigned)r.camera_frames_seen);
	put("buffer_slots=%u\n", (unsigned)r.buffer_slots);
	put("buffer_peak=%u\n", (unsigned)r.buffer_peak);
	put("sd_write_count=%u\n", (unsigned)r.sd_write_count);
	put("sd_video_writes=%u\n", (unsigned)r.sd_video_writes);
	put("sd_audio_writes=%u\n", (unsigned)r.sd_audio_writes);
	put("sd_bytes=%lu\n", (unsigned long)r.sd_bytes);
	put("sd_total_write_ms=%u\n", (unsigned)r.sd_total_write_ms);
	put("sd_avg_write_ms=%u\n", (unsigned)r.sd_avg_write_ms);
	put("sd_max_write_ms=%u\n", (unsigned)r.sd_max_write_ms);
	put("sd_slow_writes_250ms=%u\n", (unsigned)r.sd_slow_writes_250ms);
	put("sd_effective_mb_s=%u.%02u\n", (unsigned)(mbs / 100), (unsigned)(mbs % 100));
	put("sd_latency_hist_ms=<20:%u <50:%u <100:%u <250:%u <500:%u >=500:%u\n", (unsigned)r.h0, (unsigned)r.h1, (unsigned)r.h2, (unsigned)r.h3, (unsigned)r.h4, (unsigned)r.h5);
	put("aligned_writes=%u\n", (unsigned)r.aligned_writes);
	put("loop_iterations=%u\n", (unsigned)r.loop_iterations);
	put("rtc_seconds=%u\n", (unsigned)r.rtc_seconds);
	put("recording_format=%s\n", r.recording_format);
	put("audio_init=%d\n", (int)r.audio_init);
	put("mic_class=%s\n", r.mic_class);
	put("audio_callbacks=%u\n", (unsigned)r.audio_callbacks);
	put("audio_bytes=%u\n", (unsigned)r.audio_bytes);
	put("audio_samples=%u\n", (unsigned)r.audio_samples);
	put("audio_chunks=%u\n", (unsigned)r.audio_chunks);
	put("audio_peak=%u\n", (unsigned)r.audio_peak);
	put("audio_min=%d\n", (int)r.audio_min);
	put("audio_max=%d\n", (int)r.audio_max);
	put("audio_mean=%d\n", (int)r.audio_mean);
	put("audio_offset_binary=%u\n", (unsigned)r.audio_offset_binary);
	put("audio_overrun_bytes=%u\n", (unsigned)r.audio_overrun_bytes);
	put("audio_failed=%u\n", (unsigned)r.audio_failed);
	return n;
}

inline void copyStr(char *dst, size_t cap, const char *src) {
	size_t i = 0;
	for (; src[i] && i + 1 < cap; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

// Reads "key=value" lines back (numbers and the short strings). Unknown keys are ignored.
inline bool parse(const char *text, RecLog &r) {
	if (!text)
		return false;
	bool any = false;
	const char *p = text;
	while (*p) {
		const char *eol = strchr(p, '\n');
		const size_t len = eol ? (size_t)(eol - p) : strlen(p);
		const char *eq = (const char *)memchr(p, '=', len);
		if (eq) {
			const size_t klen = (size_t)(eq - p);
			char key[40];
			if (klen < sizeof(key)) {
				memcpy(key, p, klen);
				key[klen] = 0;
				char val[96];
				size_t vlen = len - klen - 1;
				if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
				memcpy(val, eq + 1, vlen);
				val[vlen] = 0;
				bool hit = false;
#define X(n) if (!hit && !strcmp(key, #n)) { r.n = (uint32_t)strtoul(val, nullptr, 10); hit = true; }
				RECLOG_U32_FIELDS(X)
#undef X
				if (!hit && !strcmp(key, "sd_bytes")) { r.sd_bytes = strtoull(val, nullptr, 10); hit = true; }
				if (!hit && !strcmp(key, "audio_min")) { r.audio_min = (int32_t)strtol(val, nullptr, 10); hit = true; }
				if (!hit && !strcmp(key, "audio_max")) { r.audio_max = (int32_t)strtol(val, nullptr, 10); hit = true; }
				if (!hit && !strcmp(key, "audio_mean")) { r.audio_mean = (int32_t)strtol(val, nullptr, 10); hit = true; }
				if (!hit && !strcmp(key, "recording_format")) { copyStr(r.recording_format, sizeof(r.recording_format), val); hit = true; }
				if (!hit && !strcmp(key, "mic_class")) { copyStr(r.mic_class, sizeof(r.mic_class), val); hit = true; }
				if (!hit && !strcmp(key, "stop_reason")) { copyStr(r.stop_reason, sizeof(r.stop_reason), val); hit = true; }
				if (!hit && !strcmp(key, "result")) { copyStr(r.result, sizeof(r.result), val); hit = true; }
				if (!hit && !strcmp(key, "file")) { copyStr(r.file, sizeof(r.file), val); hit = true; }
				if (hit) any = true;
			}
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return any;
}

} // namespace reclog
