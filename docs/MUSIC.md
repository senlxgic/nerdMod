# nerdMod Music (`music.srldr`)

Local SD-card music player, built with the legacy devkitPro toolchain like the Camera and Photos apps (it shares their user-interface layer).
**CI/static verified, NOT real-hardware verified.**

## Formats

| Format | Decoder | Notes |
|---|---|---|
| MP3 (MPEG-1/2/2.5 layer III) | [minimp3](https://github.com/lieff/minimp3), CC0 | CBR and VBR; seeking estimates the byte position from the duration (exact for CBR) and re-syncs on the next frame header |
| WAV | built in (`nmwav.h`) | PCM only, 8 or 16 bit, mono or stereo, 3-48 kHz; everything else is refused with a message |

OGG Vorbis is **not** supported (no decoder was added, so nothing claims it).

The MP3 decoder uses floating point in software (the ARM9 has no FPU). Its speed on a DSi is not measured; the player shows
`underruns N` on the top screen if the decoder ever falls behind and restarts the output from what was decoded. 128 kbps 44.1 kHz stereo
is the case to try first.

## Where music is looked for

`sd:/Music` and `sd:/_nds/nerdMod/music`, up to 4 folder levels deep, at most 150 folders and 400 tracks. Nothing else on the card is scanned.
Files are never moved or modified. The result is cached in `sd:/_nds/nerdMod/cache/music-index.txt` (path, size, duration, title, artist, album);
a file is re-read only when its path or size changed. The scan message shows `N tracks +added -removed`. **Y** in the library rescans.

Tags: ID3v2 (v2.2/2.3/2.4 text frames), then ID3v1, then the file name (`Artist - Title.mp3`). A malformed tag falls back to the file name.
No embedded cover art is decoded; the top screen shows generated art (a colour taken from the album/title).

## Playback

```
file -> 16 KiB read window -> minimp3 / WAV reader -> left ring + right ring (16384 frames each, 32 KiB) -> two looping hardware channels
```

* Whole files are never loaded. Peak RAM: rings 64 KiB + read window 16 KiB + decoder state ~7 KiB + the 400-entry track table ~100 KiB.
* The two channels are panned hard left/right. `soundEnable()` is called before the channels start; volume is the channel volume (0-127).
* The playback position comes from the msclock tick counter converted with the **real** sound-timer period (`2^24 / rate`), so the ring
  write position never drifts from the hardware read position (`musicmath.h`, host-tested).
* 6144 frames are buffered before the channels start. If the decoder falls behind the hardware (underrun) the output is stopped, the rings are
  cleared and buffering restarts from the decoded position; the count is shown.
* After the last sample 2048 frames of silence are written so the looping ring never repeats old audio.
* Music plays only inside the app. Leaving it (B, power button) stops playback; nothing keeps playing while a game runs.

## Controls

Player: A play/pause, L previous (restarts the track after 3 s), R next, Left/Right seek 10 s, Up/Down volume, X shuffle, Y repeat (off/all/one),
SELECT library, B back to the menu. Touch: the four buttons, the `[-]` `[+]` volume keys, the shuffle/repeat line, the progress bar (seek).

Library: Up/Down, L/R page, A (or a second tap on the selected row) plays, Y rescans, B returns to the player (or the menu if nothing plays).

Volume, shuffle and repeat are saved in `sd:/_nds/nerdMod/music.ini`.

## Tests

`camera/tools/hosttest` (`make test`): `test_music` covers the WAV parser (8/16 bit, mono/stereo, odd chunks, truncated and corrupt files),
ID3v1/v2, MP3 headers, a real minimp3 decode of two MP3 fixtures, the library index format, and `musicmath.h` (clock, ring, underrun, progress bar, touch).
