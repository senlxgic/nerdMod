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
| 12 | u16 | fpsNum | the REQUESTED rate: 10, 15, 20 or 30 (Phase 2C; older files: 10). Timestamps are authoritative |
| 14 | u16 | fpsDen | 1 |
| 16 | u32 | videoFrames | patched at the end |
| 20 | u32 | droppedFrames | frames that were due but could not be stored (SD too slow) |
| 24 | u32 | durationMs | patched |
| 28 | u32 | audioBytes | patched |
| 32 | u16 | audioFormat | 0 none, 1 = signed 16-bit PCM mono |
| 34 | u16 | audioRate | 16000 |
| 36 | u16 | audioChannels | 1 |
| 38 | u16 | videoFormat | 1 = RGB555 256x192, 2 = RGB332 256x192, 3 = RGB332 128x96 (formats 2 and 3 need version 2) |
| 40 | u32 | indexOffset | 0 = no index |
| 44 | u32 | indexCount | |
| 48 | u32 | flags | bit0 COMPLETE (clean stop, index valid), bit1 HAS_AUDIO, bit2 INNER_CAMERA, bit3 FRAMES_DROPPED, bit4 AUDIO_FAILED (Phase 2C: a microphone was requested but produced no data) |
| 52 | u32 | startUnix | local time at start |
| 56 | u32 | maxWriteMs | Phase 2C: slowest single SD write while recording (0 in older files) |
| 60 | u32 | capturedFrames | Phase 2C: frames delivered by the camera while recording (0 in older files) |

## Chunks

`ChunkHeader` = `u32 fourcc, u32 size, u32 timeMs, u32 reserved`.

* `VFRM` — one frame, 256×192, 2 bytes/pixel, row-major, top row first. Pixel = `1BBBBBGGGGGRRRRR` (bit 15 set; ffmpeg name `bgr555le`).
* `AUDI` — signed 16-bit little-endian PCM, mono, `audioRate` Hz. `timeMs` is the time of its first sample.
* `PAD ` — filler (Phase 2C.1). `size` bytes of zeros that readers skip. It makes every recorder write start and end on a
  512-byte (SD sector) boundary of the file.

Video chunks are in time order; audio chunks are interleaved roughly every quarter second. A reader must ignore unknown
chunk types (skip `size` bytes) and must stop quietly at a truncated or implausible chunk.

### Sector-aligned layout (Phase 2C.1)

Files written by Phase 2C.1 and later: the first 512 bytes are the 64-byte header plus one `PAD ` chunk; each video frame
is `PAD`(480 bytes of filler) + `VFRM` + 98,304 bytes of pixels = 98,816 bytes (193 sectors); each audio block is `AUDI` +
a `PAD ` chunk up to the next sector. The index points at the `VFRM` headers as before. Older files (chunks directly after
the 64-byte header, no `PAD `) read exactly as before; a reader that does not know `PAD ` and stops at it still has
the header and the index (`videoFrames`, `indexOffset`) but should be updated to skip it. The container version is still 1.

## Index

After the last chunk: `indexCount` entries of `{u32 offset, u32 timeMs}`, one per video frame (offset of its chunk header).
A file without `COMPLETE` (power loss, battery) has no index and no patched counts; it is read by scanning the chunks and
loses at most the last, partly written frame. The temporary name while recording is `REC_TEMP.nvid.tmp`; it is renamed to
`NV_YYYYMMDD_HHMMSS.nvid` after the clean stop.

## Version policy (Phase 2C; superseded by version 2 below)

Phase 2C kept the container at **version 1**. Phase 2C only gives meaning to fields that older files wrote as 0 (`fpsNum` is now the
requested rate, the two former reserved words carry recorder statistics, flag bit 4). A reader that ignores them reads
every old and new file; a Phase 2C reader treats 0 as "unknown". The video payload is still RGB555 (`videoFormat` 1):
the camera is configured to emit RGB555 directly, so recording needs no per-frame CPU conversion. Native YUV422 would be
the same 2 bytes/pixel (no smaller files) and would add a CPU conversion for the live preview, so a version-2/YUV
container was **not** introduced. `videoFormat` reserves the numbers for it should that change.

## Version 2 and video quality (Phase 2D)

Phase 2D bumps the container to **version 2**. The layout is unchanged; only `videoFormat` may now be more than RGB555:

| Quality (Camera setting) | videoFormat | picture | payload | slot (sector aligned) |
|---|---|---|---|---|
| HIGH | 1 RGB555 | 256x192, 2 B/pixel | 98,304 B | 98,816 B |
| BALANCED (default) | 2 RGB332 | 256x192, 1 B/pixel (R3 G3 B2) | 49,152 B | 49,664 B |
| SMALL | 3 RGB332 half | 128x96 (2x2 box average), shown at 2x | 12,288 B | 12,800 B |

Why: on real hardware the SD card write speed was the bottleneck of recording (about 0.16 MB/s measured with the video recorder), so fewer
bytes per frame is the only lever that does not depend on the card. The recorder converts the (effect-processed) RGB555 camera
frame into the chosen payload only when a frame is due; the header `width`/`height` always describe the stored picture. Readers
decode every format to 256x192 RGB555 (`vfmt::decode`, `videofmt.h`; Python: `decode_frame`). Version 1 files (always RGB555)
play unchanged, a version 1 header that claims another format is rejected, as is an unknown version or format. Colour precision is
reduced for BALANCED/SMALL (no dithering): gradients show banding. Index entries and time stamps are identical in all formats.

## Frame rates and timestamps

The recorder runs a fixed-point grid on the hardware millisecond clock: slot k is due at `k * 1000 / fps` ms (integer
maths, no accumulating error). A camera frame is stored when it lands within half an interval (at most 20 ms) before its slot and is
stamped with its real capture time. Nothing is duplicated or interpolated; a slot with no frame is a *dropped* frame and
is counted. Actual average rate = frames / duration (the Info page and `nerdvid_convert.py info` show it).
A camera that delivers 30 frames/s cannot give an evenly spaced 20 fps: slots then alternate 1 and 2 camera periods
(33/67 ms) - the timestamps say so honestly.

## Size and bandwidth

One frame is 98,304 B (+16 B header). With 32 KB/s of audio:

| FPS | MB/min (decimal) | time to the 1500 MiB cap |
|---|---|---|
| 10 | 61 | 25 min (30 min limit applies first) |
| 15 | 90 | 17 min |
| 20 | 120 | 13 min |
| 30 | 179 | 8.7 min |

The recorder stops by itself at 30 minutes or 1500 MiB (`MAX_FILE_BYTES`), whichever comes first. FAT32 allows 4 GiB per file and
all offsets/sizes are `u32`, so 1500 MiB keeps every offset, the index (8 bytes per frame; 30 fps x 30 min = 54,000
entries = 432 KB) and the final rename well inside the limits.

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
