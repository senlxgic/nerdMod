# Phase 2C.1 — hardware fix pass (CI/static verified, NOT real-hardware verified)

This pass reacts to the first real-DSi test of Phase 2C. Nothing here has been run on a console yet.

## Home photo
* Cause (from the owner's `photo-status.txt`): the active theme sets `RenderPhoto=0` while nerdMod "Show Photo" was ON; the Phase 2C loader skipped the picture.
* New precedence (`universal/include/common/nmphoto.h`, `ThemeConfig::photoDecision()`): Show Photo OFF → no photo; Show Photo ON → the nerdMod photo is drawn into the centre region, whatever the theme says. Macro mode and themes without a top-screen photo area (Saturn, HBL) still show none.
* The theme's own graphics are kept: the theme still chooses its top background and shoulder graphics from its own `RenderPhoto`; nothing is written to theme files and `theme.ini` is never touched. The override is runtime only.
* `photo-status.txt` now lists `showPhotoSetting`, `themeRenderPhoto`, `nerdModOverride`, `policy`, per-app registry lines and `final=… decode=…`.

## Photos tile
* `photos.srldr` is in the artifact (777,216 bytes in the Phase 2C build). CI now fails if `camera.srldr`, `photos.srldr`, `dsimenu.srldr` or `settings.srldr` is missing/empty, or if Camera/Photos are not in the built-in registry.
* The registry is the same code path as Camera. If the tile is still missing, `photo-status.txt` has a line `app photos: path=… exists=… size=… available=…` that shows which condition fails.

## Video recording
* `sd:/_nds/nerdMod/videos/last-recording.txt` is written after every recording attempt, including failed starts (`result=START_FAILED`).
* `sd_max_write_ms` is the **slowest single write()**, not an average. The average, total time inside write(), bytes and MB/s are separate fields, with a latency histogram.
* `loop_iterations` and `rtc_seconds` are logged next to `duration_ms` so a wrong millisecond clock can be told apart from a slow card.
* Writes are now sector aligned (see NERDVID.md). The Phase 2B/2C layout put a 64-byte header first, so every 98,336-byte frame write started mid-sector and libfat had to read-modify-write the first and last sector through its cache.
* Preallocation was not added: libfat's `ftruncate` / seek-past-end growth zero-fills every new sector, so it moves the same writes to the start instead of removing them. The SD speed test has a `rewrite` row (overwrite without growth) so the growth cost can be measured first.
* Capture is overlapped with the write only as far as the hardware allows: the camera transfer into a free slot is started before each SD write and finishes by DMA while the CPU is inside write(); the next transfer can only be started after write() returns. A slow write therefore costs camera frames; the 12-slot ring absorbs short stalls.

## Camera diagnostics (START in photo mode, or after a recording)
Pages: Last video · SD card · Microphone · Tools · SD speed test. Tools: **X** = SD speed test (writes `sd-benchmark.txt`), **Y** = 1 kHz test tone through the same playback path as video.

## Microphone / audio
* Classification: `MIC_INIT_FAILED`, `MIC_NO_CALLBACKS`, `MIC_CALLBACKS_BUT_ZERO_DATA` (peak < 64), `MIC_VALID_DATA`. The log also holds callbacks, bytes, samples, min/max/mean, peak, offset-binary flag and overrun.
* Fixed on the way: the audio payload is filled by the CPU but only the 16-byte chunk header was cache-flushed before the SD write; the whole audio block is flushed now. The mic callback no longer converts in place inside the buffer shared with the ARM7.
* Playback: the Camera's ARM7 clears the sound registers and only sets `SOUND_ENABLE`; the ARM9 never called `soundEnable()`, so master volume could stay 0. `audioPlay::start()` now calls it. The player shows `NO SOUND`, `SND`, `SND OK`, `SND LATE` or `CH FAIL`; the Album shows requested/actual fps and the sound bytes stored in the file.
