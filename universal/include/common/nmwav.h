// nerdMod Music: RIFF/WAVE parser for uncompressed PCM (8/16-bit, mono/stereo). Pure code, host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace nmwav {

struct Info {
	bool ok = false;
	uint16_t channels = 0;
	uint16_t bits = 0;
	uint32_t rate = 0;
	uint32_t dataOffset = 0; // file offset of the first sample
	uint32_t dataBytes = 0;	 // clamped to what the file can actually hold when fileSize is given
	const char *error = "ok";
	uint32_t frameBytes() const { return (uint32_t)channels * (bits / 8); }
	uint32_t frames() const { return frameBytes() ? dataBytes / frameBytes() : 0; }
	uint32_t durationMs() const { return rate ? (uint32_t)((uint64_t)frames() * 1000 / rate) : 0; }
};

inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// `buf` holds the first `len` bytes of the file (4 KiB is plenty); fileSize is the real file size (0 = unknown).
inline Info parse(const uint8_t *buf, size_t len, uint32_t fileSize) {
	Info r;
	if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
		r.error = "not a WAV file";
		return r;
	}
	bool haveFmt = false;
	size_t pos = 12;
	uint16_t tag = 0;
	while (pos + 8 <= len) {
		const uint32_t sz = rd32(buf + pos + 4);
		const uint8_t *body = buf + pos + 8;
		if (memcmp(buf + pos, "fmt ", 4) == 0) {
			if (sz < 16 || pos + 8 + 16 > len) {
				r.error = "bad fmt chunk";
				return r;
			}
			tag = rd16(body);
			r.channels = rd16(body + 2);
			r.rate = rd32(body + 4);
			r.bits = rd16(body + 14);
			if (tag == 0xFFFE && sz >= 26 && pos + 8 + 26 <= len)
				tag = rd16(body + 24); // WAVE_FORMAT_EXTENSIBLE: sub-format GUID starts with the real tag
			haveFmt = true;
		} else if (memcmp(buf + pos, "data", 4) == 0) {
			if (!haveFmt) {
				r.error = "data before fmt";
				return r;
			}
			if (tag != 1) {
				r.error = "not PCM";
				return r;
			}
			if (r.channels < 1 || r.channels > 2) {
				r.error = "only mono or stereo";
				return r;
			}
			if (r.bits != 8 && r.bits != 16) {
				r.error = "only 8 or 16 bit";
				return r;
			}
			if (r.rate < 4000 || r.rate > 96000) {
				r.error = "unsupported sample rate";
				return r;
			}
			r.dataOffset = (uint32_t)(pos + 8);
			r.dataBytes = sz;
			if (fileSize) {
				if (r.dataOffset >= fileSize) {
					r.error = "no audio data";
					return r;
				}
				if ((uint64_t)r.dataOffset + sz > fileSize)
					r.dataBytes = fileSize - r.dataOffset; // streaming writers leave 0 / 0xFFFFFFFF; truncated files play what exists
				if (sz == 0)
					r.dataBytes = fileSize - r.dataOffset;
			}
			r.dataBytes -= r.dataBytes % r.frameBytes();
			if (r.dataBytes == 0) {
				r.error = "no audio data";
				return r;
			}
			r.ok = true;
			return r;
		}
		const uint64_t next = (uint64_t)pos + 8 + sz + (sz & 1);
		if (next > len)
			break;
		pos = (size_t)next;
	}
	r.error = "no data chunk";
	return r;
}

// Converts `frames` PCM frames to interleaved signed 16-bit stereo.
inline void toStereo16(const Info &i, const uint8_t *src, uint32_t frames, int16_t *out) {
	for (uint32_t f = 0; f < frames; f++) {
		int l, r;
		if (i.bits == 16) {
			const uint8_t *p = src + (size_t)f * i.channels * 2;
			l = (int16_t)rd16(p);
			r = i.channels == 2 ? (int16_t)rd16(p + 2) : l;
		} else {
			const uint8_t *p = src + (size_t)f * i.channels;
			l = ((int)p[0] - 128) << 8;
			r = i.channels == 2 ? ((int)p[1] - 128) << 8 : l;
		}
		out[f * 2] = (int16_t)l;
		out[f * 2 + 1] = (int16_t)r;
	}
}

} // namespace nmwav
