/*
	Pure helpers for the SD benchmark (Phase 2D): latency samples -> min/avg/max/p50/p95/p99, throughput, and the text
	report. No hardware access, so the host tests can exercise it.
*/
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace sdbenchcalc {

constexpr uint32_t MAX_SAMPLES = 1024; // per test: 16 KiB buffers x 4 MiB = 256 writes, so this never truncates in practice

// One benchmark test. All times in microseconds internally, printed as milliseconds with one decimal.
struct Result {
	char name[20] = "";
	uint32_t bufferBytes = 0;
	uint64_t bytesWritten = 0;
	uint32_t writeCount = 0;
	uint32_t totalUs = 0; // wall time spent inside write() calls
	uint32_t minUs = 0, maxUs = 0, avgUs = 0, p50Us = 0, p95Us = 0, p99Us = 0;
	uint32_t openUs = 0, closeUs = 0, flushUs = 0;
	bool aligned = false;
	bool rewrite = false; // false = file growth, true = overwrite in place
	bool withMic = false; // microphone capture was running during the test
	uint32_t micCallbacks = 0;
	uint32_t micSamples = 0;
	bool ok = false;
	char error[28] = "";
};

// Sorts n microsecond samples in place (insertion sort for small n, shell sort otherwise) - no libc qsort dependency.
inline void sortSamples(uint32_t *a, uint32_t n) {
	for (uint32_t gap = n / 2; gap > 0; gap /= 2)
		for (uint32_t i = gap; i < n; i++) {
			const uint32_t t = a[i];
			uint32_t j = i;
			for (; j >= gap && a[j - gap] > t; j -= gap)
				a[j] = a[j - gap];
			a[j] = t;
		}
}

// Nearest-rank percentile of a SORTED array (pct 0..100).
inline uint32_t percentile(const uint32_t *sorted, uint32_t n, uint32_t pct) {
	if (!n)
		return 0;
	uint32_t rank = (uint32_t)(((uint64_t)pct * n + 99u) / 100u); // ceil(pct*n/100)
	if (rank < 1)
		rank = 1;
	if (rank > n)
		rank = n;
	return sorted[rank - 1];
}

// Fills the statistics fields of r from the (unsorted) samples. Sorts `samples` in place.
inline void summarise(Result &r, uint32_t *samples, uint32_t n) {
	if (!n) {
		r.minUs = r.maxUs = r.avgUs = r.p50Us = r.p95Us = r.p99Us = 0;
		return;
	}
	sortSamples(samples, n);
	uint64_t sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum += samples[i];
	r.minUs = samples[0];
	r.maxUs = samples[n - 1];
	r.avgUs = (uint32_t)(sum / n);
	r.p50Us = percentile(samples, n, 50);
	r.p95Us = percentile(samples, n, 95);
	r.p99Us = percentile(samples, n, 99);
}

// Decimal MB/s x100 from bytes and microseconds spent in write().
inline uint32_t mbPerSecX100(uint64_t bytes, uint32_t us) {
	return us ? (uint32_t)((bytes * 100u) / (uint64_t)us) : 0; // bytes/us == MB/s (decimal)
}

inline size_t formatResult(char *out, size_t cap, const Result &r) {
	const uint32_t mbs = mbPerSecX100(r.bytesWritten, r.totalUs);
	auto ms = [](uint32_t us, char *buf, size_t n) { snprintf(buf, n, "%u.%u", (unsigned)(us / 1000), (unsigned)((us % 1000) / 100)); };
	char avg[16], mn[16], mx[16], p50[16], p95[16], p99[16], op[16], cl[16], fl[16];
	ms(r.avgUs, avg, sizeof avg); ms(r.minUs, mn, sizeof mn); ms(r.maxUs, mx, sizeof mx);
	ms(r.p50Us, p50, sizeof p50); ms(r.p95Us, p95, sizeof p95); ms(r.p99Us, p99, sizeof p99);
	ms(r.openUs, op, sizeof op); ms(r.closeUs, cl, sizeof cl); ms(r.flushUs, fl, sizeof fl);
	const int w = snprintf(out, cap,
						   "test_name=%s buffer_size=%u bytes_written=%u write_count=%u total_ms=%u.%u avg_write_ms=%s min_write_ms=%s max_write_ms=%s "
						   "p50_ms=%s p95_ms=%s p99_ms=%s mb_s=%u.%02u aligned=%d growth_or_rewrite=%s mic_running=%d mic_callbacks=%u mic_samples=%u open_ms=%s close_ms=%s flush_ms=%s ok=%d%s%s\n",
						   r.name, (unsigned)r.bufferBytes, (unsigned)r.bytesWritten, (unsigned)r.writeCount, (unsigned)(r.totalUs / 1000), (unsigned)((r.totalUs % 1000) / 100), avg, mn,
						   mx, p50, p95, p99, (unsigned)(mbs / 100), (unsigned)(mbs % 100), (int)r.aligned, r.rewrite ? "rewrite" : "growth", (int)r.withMic, (unsigned)r.micCallbacks, (unsigned)r.micSamples, op, cl, fl, (int)r.ok,
						   r.error[0] ? " error=" : "", r.error);
	return w < 0 ? 0 : ((size_t)w < cap ? (size_t)w : cap - 1);
}

} // namespace sdbenchcalc
