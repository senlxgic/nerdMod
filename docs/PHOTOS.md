# nerdMod Photos (Phase 2C)

Status: **CI/static verified, NOT real-hardware verified.**

A built-in app (`photos.srldr`) registered in the virtual-app table (`virtualEntries.cpp`) so it appears as a normal tile next
to Camera, with an original icon. It is built from `photos/` and reuses the Camera UI layer (`camera/arm9/source/ui.cpp`), i.e.
the same display setup as the Camera Album that works on hardware.

* Categories (empty ones hidden): **Photos**, **Home photos**, **Screenshots**, **Drawings**, **Other** - see `docs/MEDIA_LAYOUT.md`.
* Formats: BMP 24/32-bit (BI_RGB; 32-bit BI_BITFIELDS BGRA), top-down or bottom-up; PNG. Scaled to fit 256x192 with the aspect kept
  (BMP is streamed row by row; PNG is size-checked first and limited to 3 megapixels / 8 MB). Anything else is reported, not guessed at.
* Screens: category list → thumbnail list (the selected picture is previewed on the top screen after the cursor rests) → viewer
  (Left/Right, L/R). **Y** options, **X** info, **A** open, **B** back; touch rows work too.
* Options: **Set as Home Photo**, **Use default home photo**, **Delete** (confirmed), **Info**.
* Not included (by design): music, drawing, notes, file manager, transfer, screenshot hotkey.

## Set as Home Photo
Writes `sd:/_nds/nerdMod/home-photo.ini`:
```
[HOME]
PATH=sd:/_nds/nerdMod/photos/NM_20261004_101500.bmp
SERIAL=7
```
and reads it back to verify. The home screen (`loadPhotoList` in `graphics.cpp`) prefers `PATH` over the folders. `SERIAL` changes
on every write, so anything that caches the decoded photo can detect a change. The picture file is not copied or modified.
"Use default home photo" writes an empty `PATH`. The new photo appears the next time the menu starts.

## Set as Wallpaper - investigated, deferred
A wallpaper would be the *background* of the DSi theme. Those backgrounds are theme assets decoded by the theme loader
(`ThemeTextures`) into several layers and palettes per theme (and custom themes such as Cinnamoroll ship their own), and the
home layout is driven by `ThemeConfig`. A nerdMod-owned overlay under the icons would need a new layer and ordering rules in
every theme's renderer, and overwriting theme files is not allowed. Because that touches verified rendering for every theme it
was **not** implemented. A safe later design: an optional bottom-screen background layer drawn before the theme's sprites,
disabled for themes that mark it unsupported.
