# nerdMod Camera

A small Nintendo DSi camera app (separate executable, so a camera problem cannot take the menu down).

## Installing / launching

`make package` builds it and places **`_nds/TWiLightMenu/camera.srldr`** in `7zfile/` next to the other
`.srldr` files, so it is updated by the usual "copy BOOT.NDS + `_nds/TWiLightMenu/*.srldr`" step.
Nothing else is installed and no user file (settings.ini, themes, boxart, ROMs, saves) is touched.

It is started from the DSi menu's **SELECT menu** (row "Open Camera", just above "Open Manual"),
exactly like the manual: `dsimenu.srldr` calls `runNdsFile("camera.srldr", ..., dsModeSwitch = false)`.
The SELECT menu is used by the DSi / Saturn / HBL themes when Settings -> "DSi/Saturn: SELECT" is set
to "SELECT menu" (the stock default is "DS Classic Menu"). The row is only shown on a DSi (not a 3DS,
not in kiosk mode) and only if `camera.srldr` exists.

On exit it starts the same menu the manual would return to (see `returnToMenu()` in `arm9/source/main.cpp`).

## DSi mode

`dsModeSwitch = false` means the loader never changes `SCFG_EXT`; the app runs with whatever the menu had
(on a normally booted DSi: `0x8307F100`, i.e. bits 31/17/16 = SCFG, camera and NDMA registers visible).
The app checks those three bits itself (`cameraHardwareAccessible()`), and the ARM7 additionally refuses
to start the sensors if its own `SCFG_EXT` reads 0. If either check fails the app shows
"This needs a Nintendo DSi running in DSi mode" and returns to the menu without touching camera registers.

## Photos

`<sd>:/_nds/nerdMod/photos/NM_YYYYMMDD_HHMMSS.bmp` (a `_N` suffix is added if the name exists).
24-bit uncompressed BMP, 640x480, 921,654 bytes.

## Memory

| buffer | size | lifetime |
| --- | --- | --- |
| capture (YUV422 640x480, 32-byte aligned) | 614,400 B heap | allocated per shot, freed after saving |
| preview pages (RGB555 256x256 bitmaps in VRAM A and B) | 2 x 128 KB VRAM | whole session |
| BMP row buffers | 2 x 1,920 B static | whole session |
| album file names | <= 512 strings | while the album is open |
