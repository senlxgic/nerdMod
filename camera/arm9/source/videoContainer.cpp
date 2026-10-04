#include "videoContainer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nvid {

namespace {

// Largest chunk payload we accept when reading (a frame is 96 KiB, audio chunks are far smaller)
constexpr u32 MAX_PAYLOAD = 1u << 20;

bool readFully(int fd, void *dst, u32 bytes) {
	u8 *p = (u8 *)dst;
	while (bytes) {
		const ssize_t n = read(fd, p, bytes);
		if (n <= 0)
			return false;
		p += n;
		bytes -= (u32)n;
	}
	return true;
}

} // namespace

// =================================================================================================
//  Writer
// =================================================================================================
Writer::~Writer() { abandon(); }

bool Writer::writeAll(const void *data, u32 bytes) {
	const u8 *p = (const u8 *)data;
	while (bytes) {
		errno = 0;
		const ssize_t n = write(fd, p, bytes);
		if (n <= 0) {
			err = errno ? errno : EIO;
			return false;
		}
		p += n;
		bytes -= (u32)n;
		position += (u32)n;
	}
	return true;
}

bool Writer::open(const std::string &path, const WriteParams &params) {
	abandon();
	index.clear();
	index.reserve(2048);
	position = 0;
	audioBytesWritten = 0;
	err = 0;

	memset(&header, 0, sizeof(header));
	header.magic = MAGIC;
	header.version = VERSION;
	header.headerSize = HEADER_SIZE;
	header.width = WIDTH;
	header.height = HEIGHT;
	header.fpsNum = params.fpsNum;
	header.fpsDen = 1;
	header.audioFormat = params.audio ? AUDIO_PCM16_MONO : AUDIO_NONE;
	header.audioRate = params.audio ? params.audioRate : 0;
	header.audioChannels = params.audio ? 1 : 0;
	header.videoFormat = VIDEO_RGB555;
	header.flags = (params.audio ? FLAG_HAS_AUDIO : 0) | (params.innerCamera ? FLAG_INNER_CAMERA : 0);
	header.startUnix = params.startUnix;

	errno = 0;
	fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		err = errno ? errno : EIO;
		return false;
	}
	if (!writeAll(&header, sizeof(header))) {
		::close(fd);
		fd = -1;
		::remove(path.c_str());
		return false;
	}
	return true;
}

bool Writer::writeChunk(u8 *chunk, u32 fourcc, u32 payloadBytes, u32 timeMs) {
	if (fd < 0)
		return false;
	ChunkHeader *h = (ChunkHeader *)chunk;
	h->fourcc = fourcc;
	h->size = payloadBytes;
	h->timeMs = timeMs;
	h->reserved = 0;
	DC_FlushRange(chunk, CHUNK_HEADER_SIZE); // anything that reads the buffer by DMA must see the header

	const u32 offset = position;
	if (!writeAll(chunk, CHUNK_HEADER_SIZE + payloadBytes))
		return false;
	if (fourcc == CHUNK_VIDEO)
		index.push_back({offset, timeMs});
	else
		audioBytesWritten += payloadBytes;
	return true;
}

bool Writer::writeVideo(u8 *chunk, u32 payloadBytes, u32 timeMs) { return writeChunk(chunk, CHUNK_VIDEO, payloadBytes, timeMs); }
bool Writer::writeAudio(u8 *chunk, u32 payloadBytes, u32 timeMs) { return writeChunk(chunk, CHUNK_AUDIO, payloadBytes, timeMs); }

bool Writer::finish(const Final &final) {
	if (fd < 0)
		return false;

	// 1. the index (may fail on a full card: the file is still readable by scanning)
	bool indexOk = true;
	const u32 indexOffset = position;
	if (!index.empty())
		indexOk = writeAll(index.data(), (u32)(index.size() * sizeof(IndexEntry)));

	// 2. the header
	header.videoFrames = (u32)index.size();
	header.droppedFrames = final.droppedFrames;
	header.durationMs = final.durationMs;
	header.audioBytes = audioBytesWritten;
	header.indexOffset = indexOk ? indexOffset : 0;
	header.indexCount = indexOk ? (u32)index.size() : 0;
	if (final.framesDropped || final.droppedFrames)
		header.flags |= FLAG_FRAMES_DROPPED;
	if (indexOk)
		header.flags |= FLAG_COMPLETE;

	bool headerOk = false;
	if (lseek(fd, 0, SEEK_SET) == 0) {
		const u32 savedPos = position;
		headerOk = writeAll(&header, sizeof(header));
		position = savedPos;
	}
	const bool closeOk = (::close(fd) == 0);
	fd = -1;
	return indexOk && headerOk && closeOk;
}

void Writer::abandon() {
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
}

// =================================================================================================
//  Reading
// =================================================================================================
bool readHeader(const std::string &path, Header &out) {
	const int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	Header h;
	bool ok = readFully(fd, &h, sizeof(h)) && h.magic == MAGIC && h.version == VERSION && h.headerSize == HEADER_SIZE && h.width == WIDTH && h.height == HEIGHT &&
			  h.videoFormat == VIDEO_RGB555;
	::close(fd);
	if (ok)
		out = h;
	return ok;
}

Reader::~Reader() { close(); }

void Reader::close() {
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
	index.clear();
}

bool Reader::open(const std::string &path) {
	close();
	fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	if (!readFully(fd, &header, sizeof(header)) || header.magic != MAGIC || header.version != VERSION || header.headerSize != HEADER_SIZE || header.width != WIDTH ||
		header.height != HEIGHT || header.videoFormat != VIDEO_RGB555) {
		close();
		return false;
	}

	const off_t fileSize = lseek(fd, 0, SEEK_END);
	if (fileSize < (off_t)HEADER_SIZE) {
		close();
		return false;
	}
	dataEnd = (u32)fileSize;

	// Use the index if the recording ended cleanly and the index is consistent, otherwise scan the chunks.
	bool haveIndex = false;
	if ((header.flags & FLAG_COMPLETE) && header.indexOffset >= HEADER_SIZE && header.indexCount > 0 &&
		(u64)header.indexOffset + (u64)header.indexCount * sizeof(IndexEntry) <= (u64)fileSize && header.indexCount < 1000000) {
		index.resize(header.indexCount);
		if (lseek(fd, header.indexOffset, SEEK_SET) == (off_t)header.indexOffset && readFully(fd, index.data(), header.indexCount * sizeof(IndexEntry))) {
			haveIndex = true;
			for (const IndexEntry &e : index) {
				if (e.offset < HEADER_SIZE || (u64)e.offset + CHUNK_HEADER_SIZE + FRAME_BYTES > header.indexOffset) {
					haveIndex = false;
					break;
				}
			}
		}
		if (haveIndex)
			dataEnd = header.indexOffset;
	}
	if (!haveIndex) {
		index.clear();
		if (!buildIndexByScanning()) {
			close();
			return false;
		}
	}
	return rewind();
}

bool Reader::buildIndexByScanning() {
	u32 pos = HEADER_SIZE;
	while (pos + CHUNK_HEADER_SIZE <= dataEnd) {
		ChunkHeader h;
		if (lseek(fd, pos, SEEK_SET) != (off_t)pos || !readFully(fd, &h, sizeof(h)))
			break;
		if ((h.fourcc != CHUNK_VIDEO && h.fourcc != CHUNK_AUDIO) || h.size > MAX_PAYLOAD || (u64)pos + CHUNK_HEADER_SIZE + h.size > dataEnd)
			break;
		if (h.fourcc == CHUNK_VIDEO) {
			if (h.size != FRAME_BYTES)
				break;
			index.push_back({pos, h.timeMs});
		}
		pos += CHUNK_HEADER_SIZE + h.size;
	}
	dataEnd = pos; // anything after the last whole chunk is not data
	return !index.empty();
}

u32 Reader::durationMs() const {
	if ((header.flags & FLAG_COMPLETE) && header.durationMs)
		return header.durationMs;
	return index.empty() ? 0 : index.back().timeMs + 100;
}

bool Reader::rewind() {
	if (fd < 0)
		return false;
	cursor = HEADER_SIZE;
	payloadLeft = 0;
	return lseek(fd, HEADER_SIZE, SEEK_SET) == (off_t)HEADER_SIZE;
}

bool Reader::seekToTime(u32 timeMs) {
	if (fd < 0 || index.empty())
		return false;
	// last index entry with time <= timeMs
	size_t lo = 0, hi = index.size();
	while (lo + 1 < hi) {
		const size_t mid = (lo + hi) / 2;
		if (index[mid].timeMs <= timeMs)
			lo = mid;
		else
			hi = mid;
	}
	cursor = index[lo].offset;
	payloadLeft = 0;
	return lseek(fd, cursor, SEEK_SET) == (off_t)cursor;
}

bool Reader::nextChunk(Chunk &chunk) {
	if (fd < 0)
		return false;
	if (payloadLeft) {
		if (!skipPayload(payloadLeft))
			return false;
	}
	if ((u64)cursor + CHUNK_HEADER_SIZE > dataEnd)
		return false;
	ChunkHeader h;
	if (!readFully(fd, &h, sizeof(h)))
		return false;
	if ((h.fourcc != CHUNK_VIDEO && h.fourcc != CHUNK_AUDIO) || h.size > MAX_PAYLOAD || (u64)cursor + CHUNK_HEADER_SIZE + h.size > dataEnd)
		return false;
	cursor += CHUNK_HEADER_SIZE;
	payloadLeft = h.size;
	chunk.fourcc = h.fourcc;
	chunk.size = h.size;
	chunk.timeMs = h.timeMs;
	return true;
}

bool Reader::readPayload(void *dst, u32 bytes) {
	if (bytes > payloadLeft || !readFully(fd, dst, bytes))
		return false;
	cursor += bytes;
	payloadLeft -= bytes;
	return true;
}

bool Reader::skipPayload(u32 bytes) {
	if (bytes > payloadLeft)
		return false;
	if (bytes && lseek(fd, bytes, SEEK_CUR) < 0)
		return false;
	cursor += bytes;
	payloadLeft -= bytes;
	return true;
}

bool Reader::readFrame(u32 n, void *dst, u32 *timeMs) {
	if (fd < 0 || n >= index.size())
		return false;
	const u32 off = index[n].offset + CHUNK_HEADER_SIZE;
	if (lseek(fd, off, SEEK_SET) != (off_t)off || !readFully(fd, dst, FRAME_BYTES))
		return false;
	if (timeMs)
		*timeMs = index[n].timeMs;
	cursor = off + FRAME_BYTES;
	payloadLeft = 0;
	return true;
}

} // namespace nvid
