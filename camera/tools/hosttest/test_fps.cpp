// Host tests of the frame-rate scheduling (fpsutil.h). Simulates a jittery camera against the grid.
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "../../arm9/source/fpsutil.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// Simulates the recorder's accept logic. cameraHz frames arrive with +-jitterMs noise for `seconds`.
struct SimResult { uint32_t stored, expected, dupes, maxGap, minGap; };
static SimResult simulate(int fps, int cameraHz, int jitterMs, int seconds, unsigned seed) {
	srand(seed);
	std::vector<uint32_t> times;
	uint32_t next = 0;
	const uint32_t total = (uint32_t)seconds * 1000u;
	const uint32_t camStep1000 = 1000000u / (uint32_t)cameraHz; // microseconds
	uint64_t t_us = 0;
	for (uint32_t now = 0; now < total;) {
		now = (uint32_t)(t_us / 1000) + (uint32_t)(jitterMs ? (rand() % (2 * jitterMs + 1)) - jitterMs : 0);
		if ((int32_t)now < 0) now = 0;
		t_us += camStep1000;
		if (now >= total) break;
		if (!fpsutil::isDue(now, next, fps)) continue;
		times.push_back(now);
		uint32_t k = fpsutil::nearestIndex(now, fps);
		if (k < next) k = next;
		next = k + 1;
	}
	SimResult r{};
	r.stored = (uint32_t)times.size();
	r.expected = fpsutil::expectedFrames(total, fps);
	r.maxGap = 0; r.minGap = 1u << 30;
	for (size_t i = 1; i < times.size(); i++) {
		uint32_t g = times[i] - times[i - 1];
		if (g > r.maxGap) r.maxGap = g;
		if (g < r.minGap) r.minGap = g;
		if (times[i] == times[i - 1]) r.dupes++;
	}
	return r;
}

int main() {
	// ---- table
	CHECK(fpsutil::valid(10) && fpsutil::valid(15) && fpsutil::valid(20) && fpsutil::valid(30));
	CHECK(!fpsutil::valid(0) && !fpsutil::valid(25) && !fpsutil::valid(60) && !fpsutil::valid(-1));
	CHECK(fpsutil::sanitize(25) == 10);
	CHECK(fpsutil::next(10) == 15 && fpsutil::next(15) == 20 && fpsutil::next(20) == 30 && fpsutil::next(30) == 10);
	CHECK(fpsutil::next(7) == 10);
	// ---- storage rates (decimal MB per minute, incl. audio)
	CHECK(fpsutil::mbPerMinute(10) == 61);
	CHECK(fpsutil::mbPerMinute(15) == 90);
	CHECK(fpsutil::mbPerMinute(20) == 120);
	CHECK(fpsutil::mbPerMinute(30) == 179);
	// ---- no drift: frame k is due exactly where the integer formula says, error < 1 ms for 30 minutes
	for (int fps : fpsutil::RATES) {
		uint32_t kMax = (uint32_t)fps * 1800u;
		uint64_t exact = (uint64_t)kMax * 1000u / (uint32_t)fps;
		CHECK(fpsutil::dueMs(kMax, fps) == exact);
		CHECK(fpsutil::dueMs(kMax, fps) == 1800000u);
		CHECK(fpsutil::nearestIndex(fpsutil::dueMs(12345, fps), fps) == 12345);
		CHECK(fpsutil::expectedFrames(1800000u, fps) == kMax);
	}
	// 30 fps: 33/34 ms spacing only
	for (uint32_t k = 0; k < 3000; k++) {
		uint32_t d = fpsutil::dueMs(k + 1, 30) - fpsutil::dueMs(k, 30);
		CHECK(d == 33 || d == 34);
	}
	// ---- simulation: a 30 Hz camera with +-4 ms jitter delivers every slot at 30 fps, no duplicates
	for (int fps : fpsutil::RATES) {
		SimResult r = simulate(fps, 30, 4, 60, 1);
		int diff = (int)r.stored - (int)r.expected;
		printf("fps %2d: stored %u expected %u gap %u..%u\n", fps, r.stored, r.expected, r.minGap, r.maxGap);
		CHECK(diff >= -2 && diff <= 1);
		CHECK(r.dupes == 0);
		CHECK(r.minGap >= (1000u / (uint32_t)fps) - 25u);
	}
	// a slow camera (15 Hz) asked for 30 fps: only 15 unique frames/s exist, nothing is invented
	{
		SimResult r = simulate(30, 15, 2, 60, 2);
		printf("30 fps from 15 Hz camera: stored %u\n", r.stored);
		CHECK(r.stored <= 15u * 60u + 2u);
		CHECK(r.stored >= 15u * 60u - 4u);
	}
	// ---- INI
	CHECK(fpsutil::parseFps("[CAMERA]\nVIDEO_FPS=20\n") == 20);
	CHECK(fpsutil::parseFps("VIDEO_FPS=30") == 30);
	CHECK(fpsutil::parseFps("VIDEO_FPS=12\n") == 10);
	CHECK(fpsutil::parseFps("VIDEO_FPS=\n") == 10);
	CHECK(fpsutil::parseFps("XVIDEO_FPS=30\n") == 10);
	CHECK(fpsutil::parseFps("") == 10);
	CHECK(fpsutil::parseFps(nullptr) == 10);
	CHECK(fpsutil::parseFps("VIDEO_FPS=99999999999999\n") == 10);
	// ---- remaining time
	CHECK(fpsutil::secondsThatFit(1500ull * 1024 * 1024, 30) > 500);
	CHECK(fpsutil::averageFpsX100(300, 10000) == 3000);
	CHECK(fpsutil::averageFpsX100(0, 0) == 0);

	printf(failures ? "%d FAILURES\n" : "fps tests OK\n", failures);
	return failures ? 1 : 0;
}
