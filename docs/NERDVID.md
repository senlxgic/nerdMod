# NERDVID (`.nvid`) — the nerdMod Camera video format

A small, append-only container that is cheap to write on the DSi and easy to convert. All numbers are little-endian.
Implemented by `camera/arm9/source/videoContainer.{h,cpp}`; converter in `tools/nerdvid-convert/`.

```
File   = Header (64 bytes) | Chunk* | Index
Chunk  = ChunkHeader (16 bytes) | payload (size bytes, always a multiple of 16)
```

## Header (64 bytes)

| offset | type | field | notes |
|---|---|---|---|
| 0 | u32 | magic | `NVID` |
| 4 | u16 | version | 1 |
| 6 | u16 | headerSize | 64 (chunks start here) |
| 8 | u16 | width | 256 |
| 10 | u16 | height | 192 |
| 12 | u16 | fpsNum | nominal rate (10); timestamps are authoritative |
| 14 | u16 | fpsDen | 1 |
| 16 | u32 | videoFrames | patched at the end |
| 20 | u32 | droppedFrames | frames that were due but could not be stored (SD too slow) |
| 24 | u32 | durationMs | patched |
| 28 | u32 | audioBytes | patched |
| 32 | u16 | audioFormat | 0 none, 1 = signed 16-bit PCM mono |
| 34 | u16 | audioRate | 16000 |
| 36 | u16 | audioChannels | 1 |
| 38 | u16 | videoFormat | 1 = RGB555 |
| 40 | u32 | indexOffset | 0 = no index |
| 44 | u32 | indexCount | |
| 48 | u32 | flags | bit0 COMPLETE (clean stop, index valid), bit1 HAS_AUDIO, bit2 INNER_CAMERA, bit3 FRAMES_DROPPED |
| 52 | u32 | startUnix | local time at start |
| 56 | u32[2] | reserved | 0 |

## Chunks

`ChunkHeader` = `u32 fourcc, u32 size, u32 timeMs, u32 reserved`.

* `VFRM` — one frame, 256×192, 2 bytes/pixel, row-major, top row first. Pixel = `1BBBBBGGGGGRRRRR` (bit 15 set; ffmpeg name `bgr555le`).
* `AUDI` — signed 16-bit little-endian PCM, mono, `audioRate` Hz. `timeMs` is the time of its first sample.

Video chunks are in time order; audio chunks are interleaved roughly every quarter second. A reader must ignore unknown
chunk types (skip `size` bytes) and must stop quietly at a truncated or implausible chunk.

## Index

After the last chunk: `indexCount` entries of `{u32 offset, u32 timeMs}`, one per video frame (offset of its chunk header).
A file without `COMPLETE` (power loss, battery) has no index and no patched counts; it is read by scanning the chunks and
loses at most the last, partly written frame. The temporary name while recording is `REC_TEMP.nvid.tmp`; it is renamed to
`NV_YYYYMMDD_HHMMSS.nvid` after the clean stop.

## Size and bandwidth

One frame is 98,304 B (+16 B header). At about 10 fps that is ≈ 0.98 MB/s of video plus 32 KB/s of audio, about 60 MB per minute.
The recorder stops by itself at 30 minutes or about 1.5 GB.

## Converting

```
python3 tools/nerdvid-convert/nerdvid_convert.py to-video   NV_20260101_120000.nvid out.mp4
python3 tools/nerdvid-convert/nerdvid_convert.py from-video clip.mp4 NV_20260101_120000.nvid
python3 tools/nerdvid-convert/nerdvid_convert.py info       NV_20260101_120000.nvid
```

Or directly with ffmpeg (video only, constant 10 fps):

```
ffmpeg -f rawvideo -pixel_format bgr555le -video_size 256x192 -framerate 10 -i frames.raw out.mp4
```
