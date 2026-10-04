# Media layout

All paths are on `sd:` (or `fat:` when not run from the SD card).

```
_nds/nerdMod/
  photos/                  Camera photos (NM_YYYYMMDD_HHMMSS.bmp, 640x480 24-bit). The Camera app still SAVES here.
    camera/                optional: a "Camera" sub-folder is also listed (Photos category)
    home/                  pictures meant for the home screen photo (png/bmp)
    screenshots/ drawings/ other/
  videos/                  NV_YYYYMMDD_HHMMSS.nvid (see NERDVID.md)
  cache/weather/           weather.ini (see WEATHER.md)
  cache/photos/            reserved for decoded-photo caches
  home-photo.ini           selected home photo (written by Photos)
  weather.ini              weather place (written by the owner)
  playstats.ini            play time, pending launch, totals
  camera.ini               Camera settings (VIDEO_FPS)
  photo-status.txt         home photo diagnostic, rewritten at every menu boot
```
Old and new locations are both read; **nothing is moved or renamed**. The home photo is also looked up in the existing TWiLight
folder `_nds/TWiLightMenu/dsimenu/photos/` (listed in Photos → Home photos).

## Storage rates (video)
See `NERDVID.md`: ≈ 61 / 90 / 120 / 179 MB per minute at 10 / 15 / 20 / 30 fps (with audio); 1500 MiB cap = 25 / 17 / 13 / 8.7 min.
