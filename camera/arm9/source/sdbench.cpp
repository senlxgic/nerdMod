#include "sdbench.h"

#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "msclock.h"
#include "reclog.h"
#include "video.h"

namespace sdbench {

namespace {

constexpr u32 TOTAL_BYTES = 3u * 1024u * 1024u;
constexpr u32 MAX_PATTERN_MS = 6000;
constexpr u32 BUF_BYTES = 262144;

u8 *buf = nullptr;

void runPattern(const std::string &path, Row &row, const char *name, u32 headerBytes, u32 chunk, bool rewrite) {
	snprintf(row.name, sizeof(row.name), "%s", name);
	int fd = rewrite ? ::open(path.c_str(), O_WRONLY) : ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return;
	if (rewrite) {
		if (lseek(fd, headerBytes, SEEK_SET) != (off_t)headerBytes) {
			::close(fd);
			return;
		}
	} else if (write(fd, buf, headerBytes) != (ssize_t)headerBytes) {
		::close(fd);
		return;
	}
	const u32 count = TOTAL_BYTES / chunk;
	const u32 started = msclock::ticks();
	row.ok = true;
	for (u32 i = 0; i < count; i++) {
		const u32 t0 = msclock::ticks();
		const ssize_t n = write(fd, buf, chunk);
		const u32 ms = msclock::toMs(msclock::ticks() - t0);
		if (n != (ssize_t)chunk) {
			row.ok = false;
			break;
		}
		row.writes++;
		row.bytes += chunk;
		row.totalMs += ms;
		if (ms > row.maxMs)
			row.maxMs = ms;
		if (msclock::toMs(msclock::ticks() - started) > MAX_PATTERN_MS)
			break;
	}
	const u32 c0 = msclock::ticks();
	::close(fd);
	row.closeMs = msclock::toMs(msclock::ticks() - c0);
}

} // namespace

u32 mbPerSecX100(const Row &r) { return reclog::mbPerSecX100(r.bytes, r.totalMs); }

void run(Report &out, void (*progress)(const char *name)) {
	out = Report();
	if (!rec::ensureVideoFolder())
		return;
	buf = (u8 *)memalign(32, BUF_BYTES);
	if (!buf)
		return;
	for (u32 i = 0; i < BUF_BYTES; i++)
		buf[i] = (u8)(i * 131u + (i >> 8));
	DC_FlushRange(buf, BUF_BYTES);

	const std::string path = rec::videoFolder() + "/SDBENCH.tmp";
	msclock::start();
	struct P { const char *name; u32 header, chunk; bool rewrite; };
	// the al98K file is kept for the rewrite pass, so "rewrite" must follow it directly
	const P patterns[PATTERNS] = {
		{"unal98K", 64, 98336, false},
		{"al98K", 512, 98816, false},
		{"rewrite", 512, 98816, true},
		{"al32K", 512, 32768, false},
		{"al256K", 512, 262144, false},
	};
	for (int i = 0; i < PATTERNS; i++) {
		if (progress)
			progress(patterns[i].name);
		runPattern(path, out.rows[i], patterns[i].name, patterns[i].header, patterns[i].chunk, patterns[i].rewrite);
		out.count++;
	}
	msclock::stop();
	remove(path.c_str());
	free(buf);
	buf = nullptr;

	// the text report
	char text[1400];
	size_t n = 0;
	n += snprintf(text + n, sizeof(text) - n, "nerdMod SD write benchmark (3 MiB per pattern, time spent inside write())\n");
	n += snprintf(text + n, sizeof(text) - n, "pattern writes bytes total_ms max_ms close_ms MB/s\n");
	for (int i = 0; i < out.count && n < sizeof(text) - 120; i++) {
		const Row &r = out.rows[i];
		const u32 mbs = mbPerSecX100(r);
		n += snprintf(text + n, sizeof(text) - n, "%s writes=%u bytes=%u total_ms=%u max_ms=%u close_ms=%u mb_s=%u.%02u ok=%d\n", r.name, (unsigned)r.writes, (unsigned)r.bytes,
					  (unsigned)r.totalMs, (unsigned)r.maxMs, (unsigned)r.closeMs, (unsigned)(mbs / 100), (unsigned)(mbs % 100), (int)r.ok);
	}
	FILE *f = fopen((rec::videoFolder() + "/sd-benchmark.txt").c_str(), "wb");
	if (f) {
		out.wroteFile = fwrite(text, 1, n, f) == n;
		fclose(f);
	}
}

} // namespace sdbench
