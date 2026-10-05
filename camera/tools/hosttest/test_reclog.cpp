#include <stdlib.h>
#include <string.h>
#include "../../arm9/source/reclog.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int main() {
	// telemetry maths
	CHECK(reclog::fpsX100(25, 15000) == 167);            // the owner's ~1.69 fps case: 25 frames in 15 s
	CHECK(reclog::fpsX100(150, 15000) == 1000);
	CHECK(reclog::fpsX100(5, 0) == 0);
	CHECK(reclog::mbPerSecX100(98816u * 100, 5000) == 197);   // 9.88 MB in 5 s of write time = 1.97 MB/s
	CHECK(reclog::mbPerSecX100(1000, 0) == 0);
	CHECK(reclog::bucketOf(0) == 0 && reclog::bucketOf(19) == 0 && reclog::bucketOf(20) == 1 && reclog::bucketOf(99) == 2);
	CHECK(reclog::bucketOf(249) == 3 && reclog::bucketOf(499) == 4 && reclog::bucketOf(989) == 5);

	reclog::RecLog r;
	r.requested_fps = 10; r.captured_frames = 25; r.duration_ms = 15000; r.dropped_frames = 125; r.camera_frames_seen = 300;
	r.buffer_slots = 12; r.buffer_peak = 12; r.sd_write_count = 40; r.sd_video_writes = 25; r.sd_audio_writes = 15;
	r.sd_bytes = 2470400 + 245760; r.sd_total_write_ms = 13000; r.sd_avg_write_ms = 325; r.sd_max_write_ms = 989;
	r.h5 = 3; r.audio_init = 0; r.audio_callbacks = 29; r.audio_bytes = 480000; r.audio_samples = 240000; r.audio_chunks = 15;
	r.audio_peak = 1234; r.audio_min = -1200; r.audio_max = 1234; r.audio_mean = -3; r.audio_failed = 0;
	snprintf(r.recording_format, sizeof r.recording_format, "NERDVID1-RGB555-A512");
	snprintf(r.mic_class, sizeof r.mic_class, "MIC_VALID_DATA");
	snprintf(r.mic_path, sizeof r.mic_path, "ndma"); r.audio_in_rate = 16364; r.audio_drains = 4000; r.audio_fell_back = 0; r.audio_expected_samples = 240000; r.audio_drift_ms = 12;
	snprintf(r.stop_reason, sizeof r.stop_reason, "USER");
	snprintf(r.result, sizeof r.result, "SAVED");
	char buf[3000];
	const size_t n = reclog::format(buf, sizeof buf, r);
	CHECK(n > 400 && n < sizeof buf);
	// every required key is present
	const char *keys[] = {"requested_fps=", "captured_frames=", "duration_ms=", "actual_fps=", "dropped_frames=", "camera_frames_seen=",
		"buffer_slots=", "buffer_peak=", "sd_write_count=", "sd_bytes=", "sd_avg_write_ms=", "sd_max_write_ms=", "sd_effective_mb_s=",
		"recording_format=", "audio_init=", "audio_samples=", "audio_chunks=", "audio_peak=", "audio_failed=", "stop_reason=", "mic_class=", "audio_callbacks=", "mic_path=ndma", "audio_in_rate=16364", "audio_drains=4000", "audio_drift_ms=12", "audio_expected_samples=240000"};
	for (const char *k : keys) { if (!strstr(buf, k)) { printf("missing key %s\n", k); failures++; } }
	CHECK(strstr(buf, "actual_fps=1.67\n"));
	CHECK(strstr(buf, "sd_max_write_ms=989\n"));
	// round trip
	reclog::RecLog b;
	CHECK(reclog::parse(buf, b));
	CHECK(!strcmp(b.mic_path, "ndma") && b.audio_in_rate == 16364 && b.audio_drift_ms == 12);
	CHECK(b.requested_fps == 10 && b.captured_frames == 25 && b.sd_max_write_ms == 989 && b.sd_bytes == r.sd_bytes);
	CHECK(b.audio_min == -1200 && b.audio_mean == -3 && !strcmp(b.mic_class, "MIC_VALID_DATA") && !strcmp(b.result, "SAVED"));
	// a tiny buffer never overflows
	char small[64];
	const size_t m = reclog::format(small, sizeof small, r);
	CHECK(m < sizeof small && small[m] == 0);
	CHECK(!reclog::parse("", b) && !reclog::parse(nullptr, b));
	// failed recording still produces a log
	reclog::RecLog f; snprintf(f.result, sizeof f.result, "START_FAILED"); snprintf(f.note, sizeof f.note, "Cannot create file");
	reclog::format(buf, sizeof buf, f);
	CHECK(strstr(buf, "result=START_FAILED") && strstr(buf, "note=Cannot create file"));
	printf(failures ? "%d FAILURES\n" : "reclog tests OK\n", failures);
	return failures;
}
