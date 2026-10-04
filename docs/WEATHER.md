# Home weather (Phase 2C) — what exists and what does not

Status: **CI/static verified, NOT real-hardware verified.** The weather card shows cached data and honest states; **no live
update can happen in this build** (see "Provider limitation").

## What is implemented
* `universal/include/common/nmweather.h` - data model, WMO-code → condition, cache file, strict response parser (rejects
  malformed, NaN, out-of-range, unknown codes; never invents a value), °C/°F text, location file parser. Host-tested.
* `universal/include/common/nmnet.h` - reusable non-blocking HTTP/1.1 GET client over an abstract `Transport`: URL checks,
  request text, response parser (Content-Length, chunked, read-until-close, size caps), timeout, cancellation. Host-tested with
  a scripted mock transport. `https://` is **refused** (no TLS is implemented or faked).
* `romsel_dsimenutheme/.../graphics/homeWidgets.cpp` - the card, the cache load (once, from RAM-sized files, no network wait),
  and a background state machine that starts one request ~3 s after the menu is up and polls it one step per frame.
  It can never delay boot: the card is drawn from the cache immediately.
* Cache: `sd:/_nds/nerdMod/cache/weather/weather.ini` (`TEMP_C10`, `CONDITION`, `TIME`, `LOCATION`, `PROVIDER`, `VERSION`).
  Shown stale with its age ("2H AGO"); offline without a cache shows `--` and `OFFLINE` (or `SET PLACE` when no place is set).
* Settings (Settings → GUI): *Home Weather* on/off, *Weather Units* (°C/°F), *Home Play Stats* on/off.

## Location (no hard-coded city, no IP geolocation)
`sd:/_nds/nerdMod/weather.ini`:
```
[WEATHER]
NAME=Lahore
LATITUDE=31.5497
LONGITUDE=74.3436
RELAY_URL=http://your-relay.example/weather     ; optional, plain HTTP
```
0,0 counts as "not set". No API key is needed or stored.

## Provider limitation (honest)
* Weather needs an Internet connection. The apps run in **DSi mode**. The toolchain here (libnds 1.8 / dswifi) drives the
  *DS-mode* Wi-Fi chip; in DSi mode the Wi-Fi is a different (SDIO) device for which no driver is present in this build.
  `UnavailableTransport` therefore reports "no network" and the card stays on its cache/`OFFLINE`.
* Modern weather APIs (e.g. Open-Meteo) are HTTPS-only. TLS 1.2+ with a certificate store is not practical on an ARM9 at
  67 MHz with no TLS library in the toolchain, so it was not attempted or faked.
* The intended path is a **relay**: a tiny plain-HTTP endpoint you control that returns
  `{"v":1,"temp_c":21.4,"code":3,"time":1760000000}` (`code` = WMO code). `RELAY_URL` above selects it. When a working
  `Transport` is added (for example a DSi Wi-Fi driver), nothing else changes: the same `Request`, parser, cache and card are used.
* Until then the cache can be filled by hand (write the `[WEATHER]` file above) - useful to see the card.

## Not claimed
Wi-Fi connection, HTTP over the air, TLS, live updates: none of it is verified, and the first two do not exist in this build.
