# Microphone and video sound (Phase 2C)

Status: **CI/static verified, NOT real-hardware verified.** Whether the DSi microphone produces sound in recordings has not
been proven on hardware; the Phase 2B player had no audio output at all, so "silent video" never tested the recorder.

## Research result: no hand-written codec code
The libnds in the pinned toolchain (devkitARM 20241104) already contains a DSi-aware microphone path on the ARM7
(`micStartRecording`, `micReadData16_TWL`, `micSetAmp_TWL` next to the NTR versions, chosen by `cdcIsAvailable()`, which needs
DSi mode and the application's DSi header flag), plus the TSC/codec access (`cdcReadReg`/`cdcWriteReg`). The register
definitions in its `nds/arm7/audio.h` are `REG_SNDEXTCNT 0x04004700`, `REG_MICCNT 0x04004600`, `REG_MICDATA 0x04004604` with
`MICCNT_FORMAT/FREQ_DIV/EMPTY/NOT_EMPTY/MORE_DATA/OVERRUN/CLEAR_FIFO/ENABLE...` (printed by a CI probe, not remembered).
Re-implementing that with guessed codec values would have been the "invented register values" the task forbids, so the recorder
uses libnds' ARM7 service (`soundMicRecord`) on a 16 kHz, 12-bit→PCM16 mono stream and the camera's own I2C code is untouched.

## What was added around it
* Sample-format guard: if the block arrives offset-binary (mean magnitude near 32768) it is converted to signed PCM16
  (`audiofmt.h`, host-tested).
* Telemetry: `Mic: Active, peak N` / `No microphone data` on the Camera diagnostics page (START in photo mode); the recording
  line shows `NoMic` after 2.5 s without data.
* Failure policy: if the mic cannot start, recording continues without audio; if it started but never delivered data, the file
  header carries `FLAG_AUDIO_FAILED` (no audio chunks are written). Nothing crashes.

## Playback
`audioPlayer.cpp`: one looping hardware channel (128 KB ring, 16 kHz PCM16 mono). Chunks are placed by their timestamp
(`audioring.h`, host-tested), the part behind the play position is zeroed (so a late SD read plays silence, never stale audio),
and late chunks are dropped. The audio clock is the master: late video frames are skipped; audio is never restarted to chase
video. Pause stops the channel; resume, ±5 s seek and replay restart it at the right media time (reading from 0.8 s earlier to
catch audio chunks that are stored ahead of their frame). Output follows the console's own routing (speaker/headphones) - the
program does not select an output.

## Open risks
* DSi-mode microphone gain/level and whether the codec is in TWL mode for this app's header are unproven on hardware.
* The sound channel in DSi mode with this ARM7 has not been heard on hardware.
