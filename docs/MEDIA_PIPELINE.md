# Camera → SD pipeline (Phase 2C profile)

Status: **CI/static verified, NOT real-hardware verified.** Numbers marked (est.) are estimates from bus clocks, not measurements.

## Every full-frame (98,304 B) copy while recording

| # | Stage | Who moves the bytes | CPU cost |
|---|---|---|---|
| 1 | Camera interface → RAM slot | NDMA channel 1 (hardware, RGB555 straight from the camera) | none |
| 2 | RAM slot → top-screen VRAM page (preview) | `dmaCopyHalfWords` (DMA 3) | none while it runs; ≈3 ms bus time (est.) |
| 3 | Effect (only when not NORMAL) | CPU, in place via a 32K-entry LUT | ≈ 49,152 lookups per frame (est. 6-8 ms); counted into drops |
| 4 | Slot → libfat → SD | `write()` of header+frame in one call, 32-byte aligned slot (libfat is expected to pass aligned multi-sector writes straight to the driver; not verified in this phase) | SD driver time, dominant |

The recorder used to copy nothing on the CPU for NORMAL; that is unchanged. Two copies that *could* exist were removed or never added:
no YUV→RGB conversion (the camera emits RGB555), and no memcpy into a write buffer (header and pixels share one slot).
What remains (2 and 4) is the minimum: one DMA for the preview and one write for the file.

## Ring

12 slots x 98,336 B = 1.18 MB (was 8 = 0.79 MB) when memory allows; at least 4 are required. 1 slot is being captured, up to 11 wait for the card:
0.37 s of buffering at 30 fps, 1.1 s at 10 fps. Peak RAM for recording: 1.18 MB + 16 KB audio chunk + 128 KB audio ring (DSi mode has 16 MB;
the camera app uses < 3 MB in total). If the queue is full when a frame is due it is dropped and counted; 30 consecutive
skips (about 1 s) or 6 writes slower than 500 ms stop the recording cleanly ("SD slow - saved").

## Writes

One `write()` per main-loop iteration (a frame, or an audio chunk once 8 KB of audio is waiting; before video at 24 KB), each
a multiple of 16 B and 32-byte aligned in RAM. Batching several frames in one write was considered and **not** done:
libfat is expected to issue multi-sector SD commands inside a single write anyway (98 KB = 192 sectors in one go), so a bigger write
saves little, while a 2-frame write would block capture twice as long. File pre-allocation was not done either: libfat
offers no public preallocation API and growing the FAT chain is already done sequentially by the writes.

## Per-frame budget at 30 fps (est.)

33 ms per frame. Slot→VRAM DMA ≈ 3 ms, camera DMA runs concurrently, SD write of 98 KB: a class-10 card on the DSi's SDMMC (≈ 8-10 MB/s
sustained, est.) takes ≈ 10-14 ms. The loop therefore has ~15 ms of slack; a slow card (≤ 3 MB/s) cannot reach 30 fps and will
show as drops. That is what the Info page ("max SD write") and the diagnostics page measure.

## Filters at high FPS

Only NORMAL / MONO / SEPIA / NEGATIVE work in video (unchanged). At 30 fps the LUT pass costs a visible share of the 33 ms
budget; frames lost to it show up in "Dropped" like any other drop. Photo-only effects remain photo-only.
