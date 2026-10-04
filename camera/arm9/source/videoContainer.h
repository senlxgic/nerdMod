/*
	NERDVID (".nvid") - the nerdMod Camera video container.

	A small, self-describing, append-only format that is cheap to write sequentially on an SD card and easy to
	convert (see tools/nerdvid-convert/ and docs/NERDVID.md). All numbers are little-endian.

	File    = Header (64 bytes) | Chunk* | Index
	Header  = see struct nvid::Header (fields marked "patched" are written when the recording ends)
	Chunk   = ChunkHeader (16 bytes) | payload (size bytes, always a multiple of 16)
	            'VFRM'  one video frame: 256x192, RGB555, 2 bytes per pixel, row-major, top row first
			             (bit 15 is set; the colour is bits 0-4 R, 5-9 G, 10-14 B)
	            'AUDI'  audio: signed 16-bit little-endian PCM, mono, header.audioRate Hz
	Index   = one entry (u32 chunk offset, u32 time in ms) per video frame, at header.indexOffset

	Chunks are in the order they were written, which is time order for video; audio chunks are interleaved
	roughly every half second. timeMs of a video chunk is when the frame was captured, timeMs of an audio chunk is
	the time of its first sample. Because every chunk is self-describing, a file that was cut short (no index,
	header.flags & FLAG_COMPLETE == 0) can still be read by scanning the chunks.
*/
#pragma once

#include <nds.h>

#include <string>
#include <vector>

namespace nvid {

constexpr u32 FOURCC(char a, char b, char c, char d) { return (u32)(u8)a | ((u32)(u8)b << 8) | ((u32)(u8)c << 16) | ((u32)(u8)d << 24); }

constexpr u32 MAGIC = FOURCC('N', 'V', 'I', 'D');
constexpr u16 VERSION = 1;
constexpr u32 HEADER_SIZE = 64;
constexpr u32 CHUNK_HEADER_SIZE = 16;
constexpr u32 CHUNK_VIDEO = FOURCC('V', 'F', 'R', 'M');
constexpr u32 CHUNK_AUDIO = FOURCC('A', 'U', 'D', 'I');

constexpr u16 WIDTH = 256;
constexpr u16 HEIGHT = 192;
constexpr u32 FRAME_BYTES = WIDTH * HEIGHT * 2; // 98304

constexpr u16 VIDEO_RGB555 = 1;
constexpr u16 AUDIO_NONE = 0;
constexpr u16 AUDIO_PCM16_MONO = 1;

constexpr u32 FLAG_COMPLETE = 1u << 0;	  // the recording ended cleanly and the index is valid
constexpr u32 FLAG_HAS_AUDIO = 1u << 1;
constexpr u32 FLAG_INNER_CAMERA = 1u << 2;
constexpr u32 FLAG_FRAMES_DROPPED = 1u << 3; // the SD card could not keep up at some point

struct __attribute__((packed)) Header {
	u32 magic;			// 'NVID'
	u16 version;		// 1
	u16 headerSize;		// 64
	u16 width;
	u16 height;
	u16 fpsNum;			// nominal frame rate = fpsNum / fpsDen (timestamps are authoritative)
	u16 fpsDen;
	u32 videoFrames;	// patched
	u32 droppedFrames;	// patched: frames that were due but could not be stored
	u32 durationMs;		// patched
	u32 audioBytes;		// patched
	u16 audioFormat;
	u16 audioRate;
	u16 audioChannels;
	u16 videoFormat;
	u32 indexOffset;	// patched (0 = no index)
	u32 indexCount;		// patched
	u32 flags;			// patched
	u32 startUnix;		// local time the recording started (seconds since 1970)
	u32 reserved[2];
};
static_assert(sizeof(Header) == HEADER_SIZE, "NVID header must be 64 bytes");

struct __attribute__((packed)) ChunkHeader {
	u32 fourcc;
	u32 size;		// payload bytes
	u32 timeMs;
	u32 reserved;
};
static_assert(sizeof(ChunkHeader) == CHUNK_HEADER_SIZE, "NVID chunk header must be 16 bytes");

struct IndexEntry {
	u32 offset;
	u32 timeMs;
};

// ---- writing ---------------------------------------------------------------------------------------
struct WriteParams {
	bool innerCamera = false;
	bool audio = false;
	u16 audioRate = 16000;
	u16 fpsNum = 10;
	u32 startUnix = 0;
};

struct Final {
	u32 durationMs = 0;
	u32 droppedFrames = 0;
	bool framesDropped = false;
};

class Writer {
  public:
	Writer() = default;
	~Writer();
	Writer(const Writer &) = delete;
	Writer &operator=(const Writer &) = delete;

	bool open(const std::string &path, const WriteParams &params);
	// `chunk` points at CHUNK_HEADER_SIZE bytes of room followed by `payloadBytes` of payload. The header is filled in
	// here, then header + payload go to the card in one sequential write.
	bool writeVideo(u8 *chunk, u32 payloadBytes, u32 timeMs);
	bool writeAudio(u8 *chunk, u32 payloadBytes, u32 timeMs);
	// Writes the index, patches the header and closes the file. Even if the index cannot be written (card full) the header
	// is still patched with the counts, so the file stays readable by scanning.
	bool finish(const Final &final);
	// Closes without finalising (the header still says "incomplete").
	void abandon();

	bool isOpen() const { return fd >= 0; }
	u32 frames() const { return (u32)index.size(); }
	u32 audioBytes() const { return audioBytesWritten; }
	u32 bytesWritten() const { return position; }
	int lastError() const { return err; }

  private:
	bool writeChunk(u8 *chunk, u32 fourcc, u32 payloadBytes, u32 timeMs);
	bool writeAll(const void *data, u32 bytes);

	int fd = -1;
	Header header;
	u32 position = 0;
	u32 audioBytesWritten = 0;
	int err = 0;
	std::vector<IndexEntry> index;
};

// ---- reading ---------------------------------------------------------------------------------------
bool readHeader(const std::string &path, Header &out);

class Reader {
  public:
	Reader() = default;
	~Reader();
	Reader(const Reader &) = delete;
	Reader &operator=(const Reader &) = delete;

	bool open(const std::string &path);
	void close();
	const Header &info() const { return header; }
	bool hasAudio() const { return header.audioFormat == AUDIO_PCM16_MONO; }
	u32 frameCount() const { return (u32)index.size(); }
	u32 durationMs() const;

	// Positions the read cursor at the first video frame at or before `timeMs` (and the audio that follows it).
	bool seekToTime(u32 timeMs);
	bool rewind();

	struct Chunk {
		u32 fourcc = 0;
		u32 size = 0;
		u32 timeMs = 0;
	};
	// Reads the next chunk header; false at the end of the data. The payload must then be consumed with
	// readPayload() or skipPayload().
	bool nextChunk(Chunk &chunk);
	bool readPayload(void *dst, u32 bytes);
	bool skipPayload(u32 bytes);
	// Frame `n` of the index (for thumbnails): reads its pixels into dst (FRAME_BYTES).
	bool readFrame(u32 n, void *dst, u32 *timeMs = nullptr);

  private:
	bool buildIndexByScanning();

	int fd = -1;
	Header header;
	u32 dataEnd = 0;
	u32 cursor = 0;		// file offset of the next unread byte
	u32 payloadLeft = 0; // unread bytes of the current chunk
	std::vector<IndexEntry> index;
};

} // namespace nvid
