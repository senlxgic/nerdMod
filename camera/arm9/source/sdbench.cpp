#include "sdbench.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "audioRecorder.h"
#include "msclock.h"
#include "reclog.h"
#include "video.h"

namespace sdbench {

namespace {

constexpr u32 TOTAL_BYTES = 4u * 1024u * 1024u;
constexpr u32 MAX_TEST_MS = 6000;
constexpr u32 BUF_BYTES = 512u * 1024u;

u8 *buf = nullptr;
u32 samples[sdbenchcalc::MAX_SAMPLES];

std::string baseDir; // <root>/_nds/nerdMod

struct Cfg {
	const char *name;
	u32 header;	 // bytes written first (not timed): 512 = sector aligned layout, 64 = the old unaligned layout
	u32 chunk;
	bool rewrite; // reopen the file of the previous growth test and overwrite it
	bool mic;
};

bool writeText(const std::string &path, const char *text, size_t n, int &err) {
	err = 0;
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) {
		err = errno;
		return false;
	}
	const bool ok = fwrite(text, 1, n, f) == n;
	if (!ok)
		err = errno;
	const bool closed = fclose(f) == 0;
	if (!closed && !err)
		err = errno;
	return ok && closed;
}

void setError(Report &out, const char *what, int err) {
	snprintf(out.writeError, sizeof(out.writeError), "%s err=%d", what, err);
}

void runTest(const std::string &tmp, sdbenchcalc::Result &r, const Cfg &c, bool (*cancel)()) {
	snprintf(r.name, sizeof(r.name), "%s", c.name);
	r.bufferBytes = c.chunk;
	r.aligned = (c.header % 512 == 0) && (c.chunk % 512 == 0);
	r.rewrite = c.rewrite;
	r.withMic = c.mic;

	bool micOn = false;
	if (c.mic)
		micOn = audioRec::start();
	const u32 cb0 = audioRec::callbacks(), sm0 = audioRec::sampleCount();

	const u32 o0 = msclock::ticks();
	int fd = c.rewrite ? ::open(tmp.c_str(), O_WRONLY) : ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	r.openUs = msclock::toUs(msclock::ticks() - o0);
	if (fd < 0) {
		snprintf(r.error, sizeof(r.error), "open errno %d", errno);
		if (micOn)
			audioRec::stop();
		return;
	}
	bool good = true;
	if (c.rewrite) {
		if (lseek(fd, c.header, SEEK_SET) != (off_t)c.header) {
			snprintf(r.error, sizeof(r.error), "seek errno %d", errno);
			good = false;
		}
	} else if (write(fd, buf, c.header) != (ssize_t)c.header) {
		snprintf(r.error, sizeof(r.error), "header errno %d", errno);
		good = false;
	}

	u32 n = 0;
	if (good) {
		const u32 count = TOTAL_BYTES / c.chunk;
		const u32 started = msclock::ticks();
		for (u32 i = 0; i < count; i++) {
			const u32 t0 = msclock::ticks();
			const ssize_t w = write(fd, buf, c.chunk);
			const u32 us = msclock::toUs(msclock::ticks() - t0);
			if (w != (ssize_t)c.chunk) {
				snprintf(r.error, sizeof(r.error), "write errno %d", errno);
				good = false;
				break;
			}
			if (n < sdbenchcalc::MAX_SAMPLES)
				samples[n++] = us;
			r.writeCount++;
			r.bytesWritten += c.chunk;
			r.totalUs += us;
			if (msclock::toMs(msclock::ticks() - started) > MAX_TEST_MS)
				break;
			if (cancel && cancel())
				break;
		}
	}
	const u32 f0 = msclock::ticks();
	fsync(fd);
	r.flushUs = msclock::toUs(msclock::ticks() - f0);
	const u32 c0 = msclock::ticks();
	::close(fd);
	r.closeUs = msclock::toUs(msclock::ticks() - c0);

	r.micCallbacks = audioRec::callbacks() - cb0;
	r.micSamples = audioRec::sampleCount() - sm0;
	if (micOn)
		audioRec::stop();
	sdbenchcalc::summarise(r, samples, n);
	r.ok = good && r.writeCount > 0;
}

} // namespace

u32 mbPerSecX100(const sdbenchcalc::Result &r) { return sdbenchcalc::mbPerSecX100(r.bytesWritten, r.totalUs); }

void run(Report &out, void (*progress)(const char *name, int index, int total), bool (*cancel)()) {
	out = Report();
	// derive <root>/_nds/nerdMod from the video folder (".../_nds/nerdMod/videos")
	std::string videos = rec::videoFolder();
	const size_t cut = videos.rfind("/videos");
	baseDir = cut == std::string::npos ? videos : videos.substr(0, cut);
	const size_t colon = baseDir.find(':');
	snprintf(out.device, sizeof(out.device), "%s", colon == std::string::npos ? "?" : baseDir.substr(0, colon).c_str());
	snprintf(out.filesystem, sizeof(out.filesystem), "FAT (libfat)");
	snprintf(out.path, sizeof(out.path), "%s/sd-benchmark.txt", baseDir.c_str());

	// folders, then the report stub FIRST so the file exists even if a test hangs or the power dies
	const std::string root = baseDir.substr(0, baseDir.find("/_nds"));
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir(baseDir.c_str(), 0777);
	mkdir((baseDir + "/cache").c_str(), 0777);
	int err = 0;
	{
		const char stub[] = "nerdMod SD benchmark\nstatus=started (not finished)\n";
		if (!writeText(out.path, stub, sizeof(stub) - 1, err))
			setError(out, "stub write failed", err);
	}

	struct statvfs sv;
	if (statvfs((baseDir).c_str(), &sv) == 0)
		out.clusterBytes = (u32)sv.f_bsize;

	buf = (u8 *)memalign(32, BUF_BYTES);
	if (!buf) {
		snprintf(out.writeError, sizeof(out.writeError), "no memory for buffers");
		return;
	}
	for (u32 i = 0; i < BUF_BYTES; i++)
		buf[i] = (u8)(i * 131u + (i >> 8));
	DC_FlushRange(buf, BUF_BYTES);

	const std::string tmp = baseDir + "/cache/SDBENCH.tmp";
	msclock::start();
	// A rewrite test follows the growth test of the same shape and overwrites the file it just made.
	const Cfg cfgs[] = {
		{"g16K", 512, 16384, false, false},		   {"g32K", 512, 32768, false, false},	   {"g64K", 512, 65536, false, false},
		{"g96K", 512, 98304, false, false},		   {"g98K", 512, 98816, false, false},	   {"rw98K", 512, 98816, true, false},
		{"g128K", 512, 131072, false, false},	   {"g256K", 512, 262144, false, false},   {"g512K", 512, 524288, false, false},
		{"un98K", 64, 98336, false, false},		   {"g98K+mic", 512, 98816, false, true},  {"g32K+mic", 512, 32768, false, true},
		{"rw98K+mic", 512, 98816, true, true},
	};
	const int total = (int)(sizeof(cfgs) / sizeof(cfgs[0]));
	for (int i = 0; i < total && i < MAX_TESTS; i++) {
		if (progress)
			progress(cfgs[i].name, i, total);
		// rw98K+mic needs a file made by a growth test: make it quickly if the previous test was not one
		if (cfgs[i].rewrite && i > 0 && cfgs[i - 1].rewrite == false && cfgs[i - 1].chunk != cfgs[i].chunk) {
			Cfg prep = {"prep", 512, 98816, false, false};
			sdbenchcalc::Result scratch;
			runTest(tmp, scratch, prep, cancel);
		}
		runTest(tmp, out.tests[out.count], cfgs[i], cancel);
		out.count++;
		if (cancel && cancel()) {
			out.cancelled = true;
			break;
		}
	}
	msclock::stop();
	remove(tmp.c_str());
	free(buf);
	buf = nullptr;

	// the report
	char *text = (char *)malloc(6000);
	if (!text) {
		snprintf(out.writeError, sizeof(out.writeError), "no memory for report");
		return;
	}
	size_t n = 0;
	n += snprintf(text + n, 6000 - n, "nerdMod SD benchmark\nstatus=%s\ndevice=%s\nfilesystem=%s\ncluster_size=%u\ntotal_per_test=%u\nrecorder_uses=write()+one close, no per-write fsync\n",
				  out.cancelled ? "cancelled" : "finished", out.device, out.filesystem, (unsigned)out.clusterBytes, (unsigned)TOTAL_BYTES);
	for (int i = 0; i < out.count && n < 6000 - 520; i++)
		n += sdbenchcalc::formatResult(text + n, 6000 - n, out.tests[i]);
	out.wroteReport = writeText(out.path, text, n, err);
	if (!out.wroteReport)
		setError(out, "report write failed", err);
	else
		out.writeError[0] = 0;
	free(text);
}

} // namespace sdbench
