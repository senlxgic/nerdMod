# Video recorder: why Phase 2C.1 recorded 1.6 fps, and what Phase 2D changed

Real DSi log of the 10 FPS test (Phase 2C.1): `captured_frames=15`, `duration_ms=9197`, `buffer_slots=12`, `buffer_peak=1`,
`sd_avg_write_ms=561`, `sd_max_write_ms=988`, `sd_effective_mb_s=0.16`, `stop_reason=TOO_SLOW`.

## What the code does (main loop, `camera/arm9/source/main.cpp`)

One iteration = (1) if the camera DMA finished: `rec::frameCaptured()` hands the frame to the ring, (2) **start the next camera
transfer into a free slot**, (3) `rec::pump()`: at most one blocking SD `write()`.

* The camera DMA (NDMA channel 1) is **one-shot**: it transfers exactly one frame (`TCNT` = one frame) and stops. The next frame is requested
  only when the loop gets back to step (2). A write of 561 ms therefore means the loop produces **one frame per ~561 ms**, whatever the
  ring size. That is why `buffer_peak` was 1: every iteration put one frame into the ring and one write took it out. The 12 slots were never needed
  because the producer could not run ahead of the consumer; it is not that the stalls were absorbed.
* So the ring cannot raise throughput. It could only smooth jitter, and only if the producer ran on its own (multi-frame NDMA / an IRQ that re-arms it).
  Even then the **sink** decides: 98,816 bytes in 561 ms is 0.16-0.18 MB/s. Raw RGB555 at 10 fps needs 0.98 MB/s, so a ring could not have saved 10 fps.

## Which side is the limit

* If `Camera > Settings > SD Speed Test` (`sd:/_nds/nerdMod/sd-benchmark.txt`) shows several MB/s for the same block sizes, the problem is in the recorder
  path (file growth pattern, the per-write bookkeeping), not in the card.
* If the benchmark itself shows about 0.16 MB/s, the limit is below the Camera pipeline (libfat / SD driver / card), and only fewer bytes per frame help.

The benchmark was written to separate exactly these cases (growth vs rewrite of an existing file, 16 KiB - 512 KiB blocks, p50/p95/p99, open/flush/close
times, and a run that interleaves microphone capture). It was **not yet run on hardware**, so which case applies is open until the owner sends `sd-benchmark.txt`.

## What Phase 2D changed for recording

* Fewer bytes per frame (container version 2, see `NERDVID.md`): HIGH 98 KB, BALANCED 49 KB (default), SMALL 12 KB. Old files still play.
* Truthful metrics: unique camera frames only (nothing duplicated or interpolated), dropped slots counted, requested vs actual fps, SD MB/s and write latency
  histogram in `last-recording.txt` and the Video Info page.
* Visible FPS selector (10/15/20/30) and the Settings menu; the benchmark is a touch button, Y stays Album.
* Microphone: ARM7 hardware-FIFO capture with NDMA into a shared buffer plus an ARM9 resampling ring, so audio no longer depends on the main loop servicing a callback.

## What was NOT done (and why)

An autonomous multi-frame camera DMA (repeat / chained NDMA, or an IRQ re-arm) was not implemented. The NDMA repeat/reload semantics of the camera start
mode could not be confirmed from documentation available here, a wrong guess breaks frame alignment silently, and without the benchmark result it is not known
that the sink can use the extra frames. It is the first thing to revisit if the benchmark shows a fast card but slow recording.
