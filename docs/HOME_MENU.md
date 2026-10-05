# nerdMod home menu notes (Phase 2B.1)

## Top-screen photo (centre of the top screen)
* Put pictures in `sd:/_nds/TWiLightMenu/dsimenu/photos/` (the flashcard path is used when the menu runs from it).
* Formats: **PNG** and **uncompressed 24/32-bit BMP**, any capitalisation of the extension.
* Any size is accepted. Pictures larger than the 208x156 frame are scaled down to fit (aspect kept); smaller ones are centred.
* If several pictures are present one is picked at random at every menu start. A file that cannot be read is skipped and the next one is tried; if none works the nds-bootstrap screenshot / built-in picture is used.
* After a start of the menu, `sd:/_nds/nerdMod/photo-status.txt` says which folders were looked at, which pictures were found, which one is shown and why a picture was skipped.
* Precedence (Phase 2C.1): "Show photo" OFF → no photo. "Show photo" ON → the photo is drawn even if the theme says `RenderPhoto=0` (a runtime override; the theme files are never changed and the theme keeps its own top background). Macro mode and the Saturn/HBL themes have no photo area.

## Tile context menu (UP on a tile; DSi and 3DS themes)
| Tile | Entries |
|---|---|
| Game | Play, View Info, Move (Custom sort order only), Delete / Hide, Game Settings |
| Folder | Open, Move (Custom sort order only), Hide, Delete (empty) |
| Camera (built-in) | Open, View Info |
| Empty slot | Create Folder, or Browse Folders / Show Game Library |

A/touch chooses, B/outside cancels. *Delete / Hide* opens the existing confirmation dialog (title, file name, A = delete the file only, Y = hide, B = cancel); saves are never touched. Folders can only be deleted when they are empty. *Move* uses the existing custom-order list (`gameorder.ini`); no ROM is moved on the card.

## Selected-game art (left lane of the photo frame)
Box art (`_nds/TWiLightMenu/boxart/<file>.png` or `<TID>.png`), else the banner icon enlarged, else a generated placeholder with the system name. It is drawn 0.2 s after the cursor comes to rest, cached for the last four games, and the photo underneath is restored from the photo's own buffer. Only the DSi theme with the photo frame on uses it; other themes keep the classic centred box art.

## Home library
Games from the ROM roots are listed directly in the home area next to Camera: `/roms`, `/games`, `/nds`, the default start folder and the known ROM-set folders `$NDS $DSI $DSIWARE $GBA $GB $GBC $NES $FDS $SNES $SFC $SMS $GG $GEN $MD $A26 $A52 $A78 $COL $M5 $INT $MSX $PCE $WS $NGP $SG $SC $PLG $XEX $ATR` (only these names, never every `$` folder). Nothing is moved. *Browse Folders* (UP menu) switches the current session to plain folders; *Show Game Library* returns. The Settings > Home > "Game library view" option can be set back to *Folders*.

## Camera effects
NORMAL, MONO, SEPIA, NEGATIVE, COOL, WARM, POSTERIZE, HIGH CONTRAST, MIRROR. D-pad LEFT/RIGHT or a tap on the status bar changes the effect. The live preview and the saved photo carry it. Video supports NORMAL, MONO, SEPIA and NEGATIVE; switching to video drops any other effect.

## Phase 2C additions
* **Home photo folders.** Pictures are also read from `sd:/_nds/nerdMod/photos/home/`. A picture chosen with *Photos > Set as Home Photo* is stored as `sd:/_nds/nerdMod/home-photo.ini` (`PATH=`, `SERIAL=`) and is read first. `photo-status.txt` is written on every menu start (also when nothing is shown) and says why.
* **Play Stats card (left of the photo).** Total play time, launches and games, from `sd:/_nds/nerdMod/playstats.ini`. Time is credited when the menu starts again after a game (RTC based; sessions over 12 h or with an unset/backwards clock count as 0). Switch: Settings > Home > Home Play Stats.
* **Weather card (right of the photo).** Shows the cached reading, `--`, OFFLINE or SET PLACE. It never delays boot. No live update is possible in this build (see `docs/WEATHER.md`). Switches: Home Weather, Weather in Fahrenheit.
* **Selected game caption.** Under the game art: launch count and play time.
* The widgets draw whenever the photo is shown (see the precedence above).
