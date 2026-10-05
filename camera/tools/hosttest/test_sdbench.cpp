#include <stdlib.h>
#include "../../arm9/source/sdbenchcalc.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace sdbenchcalc;
int main() {
	// percentile: nearest rank on 1..100 microsecond samples
	uint32_t s[100];
	for (int i = 0; i < 100; i++) s[99 - i] = (uint32_t)(i + 1) * 1000; // reversed on purpose
	Result r;
	summarise(r, s, 100);
	CHECK(r.minUs == 1000 && r.maxUs == 100000);
	CHECK(r.p50Us == 50000 && r.p95Us == 95000 && r.p99Us == 99000);
	CHECK(r.avgUs == 50500);
	// tiny sets
	uint32_t one[1] = {777};
	Result r1; summarise(r1, one, 1);
	CHECK(r1.minUs == 777 && r1.p99Us == 777 && r1.p50Us == 777);
	uint32_t three[3] = {30, 10, 20};
	Result r3; summarise(r3, three, 3);
	CHECK(r3.p50Us == 20 && r3.p95Us == 30 && r3.p99Us == 30);
	Result r0; summarise(r0, nullptr, 0);
	CHECK(r0.maxUs == 0);
	// throughput: 98816 bytes in 100000 us = 0.988 MB/s; the owner's real case 98816 B in 600 ms = 0.16 MB/s
	CHECK(mbPerSecX100(98816, 100000) == 98);
	CHECK(mbPerSecX100(98816ull * 10, 6000000) == 16);
	CHECK(mbPerSecX100(4u << 20, 0) == 0);
	// a large sorted-input and many-equal-values input must not misbehave
	static uint32_t big[1024];
	for (int i = 0; i < 1024; i++) big[i] = (uint32_t)(i % 7);
	Result rb; summarise(rb, big, 1024);
	CHECK(rb.minUs == 0 && rb.maxUs == 6 && rb.p50Us == 3);
	// report line has every field the owner asked for
	Result f;
	snprintf(f.name, sizeof f.name, "g98K");
	f.bufferBytes = 98816; f.bytesWritten = 98816ull * 40; f.writeCount = 40; f.totalUs = 3950000;
	f.minUs = 60000; f.maxUs = 190000; f.avgUs = 98750; f.p50Us = 95000; f.p95Us = 150000; f.p99Us = 190000;
	f.openUs = 12000; f.closeUs = 8000; f.flushUs = 4000; f.aligned = true; f.ok = true; f.micCallbacks = 9; f.micSamples = 36864;
	char line[600];
	const size_t n = formatResult(line, sizeof line, f);
	CHECK(n > 200 && n < sizeof line);
	const char *keys[] = {"test_name=g98K", "buffer_size=98816", "bytes_written=3952640", "write_count=40", "total_ms=3950.0", "avg_write_ms=98.7", "min_write_ms=60.0",
						  "max_write_ms=190.0", "p50_ms=95.0", "p95_ms=150.0", "p99_ms=190.0", "mb_s=1.00", "aligned=1", "growth_or_rewrite=growth", "mic_running=0",
						  "mic_callbacks=9", "mic_samples=36864", "open_ms=12.0", "close_ms=8.0", "flush_ms=4.0", "ok=1"};
	for (const char *k : keys)
		if (!strstr(line, k)) { printf("missing key %s in: %s\n", k, line); failures++; }
	// truncation is safe
	char tiny[40];
	const size_t t = formatResult(tiny, sizeof tiny, f);
	CHECK(t < sizeof tiny && tiny[t] == 0);
	if (failures) { printf("%d failures\n", failures); return 1; }
	printf("test_sdbench OK\n");
	return 0;
}
