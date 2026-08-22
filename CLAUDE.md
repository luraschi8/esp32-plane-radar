# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Arduino/PlatformIO firmware for an **ESP32-C3 Super Mini** driving a **1.28″ round GC9A01 SPI display (240×240)**.
It renders a live ADS-B radar (aircraft from `opendata.adsb.fi`) centered on a user-configured lat/lon, with
WiFiManager for Wi-Fi + settings via a captive/LAN portal. Upstream: `MatixYo/ESP32-Plane-Radar`.

## Commands

`pio` is usually **not on PATH** on this machine — use `~/.platformio/penv/bin/pio` (the `scripts/merge-firmware.sh`
helper falls back to it automatically).

```bash
pio run -e supermini                 # build (only env: supermini)
pio run -t upload -e supermini       # flash over USB-CDC
pio device monitor                   # serial, 115200 baud
pio run -t merge -e supermini        # -> .pio/build/supermini/firmware-merged.bin
./scripts/merge-firmware.sh          # build + merge -> release/plane-radar-merged.bin
./scripts/merge-firmware.sh --no-build
python3 scripts/build_large_airports.py   # regenerate the embedded runway dataset from OurAirports
```

**Run `pio test -e native` before and after any change** — 105 host-side tests across six suites, ~3 s:
`test_geo` (projection, checked against the API's own dst/dir), `test_settings` (presets, units, NVS),
`test_render_policy` (the render state machine), `test_adsb` (the whole fetch/parse pipeline against real
captured payloads), `test_display` (rendering and runway overlay via a recording-canvas LovyanGFX mock), and
`test_wifi` (BOOT button, credential reset, force-portal flag).

`test/` is never compiled by `pio run`, so tests cost nothing in flash. Suites include the shipped `.cpp`
directly so file-local logic is reachable, which means **file-statics persist between tests** — give each test
an explicit starting state rather than relying on order. There is no linter or formatter.

Two things the host suite cannot see, so do not assume them covered: it runs the **bitmap-font fallback**
(`g_font_is_smooth = false`) while the device ships the VLW smooth font as its primary path, and the mock
canvas records draw calls without rasterising them — geometry is checked, appearance is not. Verification = a clean build with no `src/`-or-
`include/` warnings + flash/RAM fit in the size report + the on-hardware checklist. **`OPS.md` is the full
build / verify / flash / troubleshooting reference — read it before doing any of those.** Current baseline:
RAM 16.8% (55012 B static), Flash 39.7% (1247512 B of 3 MB).

Do not reintroduce a `namespace fonts = lgfx::v1::fonts;` alias in any file: LovyanGFX >= 1.2.x already declares
a global `namespace fonts` plus `using namespace fonts;` in `lgfx_fonts.hpp`, so the alias is a redeclaration
error and `fonts::FreeSansBold12pt7b` etc. resolve without it. (This broke the build on `^1.2.7` -> 1.2.27.)

## Architecture

Arduino `setup()`/`loop()` on the main task, plus **one** FreeRTOS task for ADS-B fetching (see below). No dynamic allocation in the hot path.
Layering is by directory, headers in `include/<layer>/`, sources in `src/<layer>/`:

- **`hardware/`** — `LGFX` LovyanGFX device (GC9A01 over SPI2, pins from `config.h`), plus the embedded
  anti-aliased VLW font (`data/ui_font.vlw`, linked via `board_build.embed_files` and reached through
  `_binary_data_ui_font_vlw_start`). Every text-drawing site must go through `display_font.h`
  (`displayFontSetSmoothSize` / `displayFontSetBitmap`) because the VLW load can fail and the code falls
  back to bitmap `GFXfont`s at runtime.
- **`services/`** — `wifi_setup` (WiFiManager, BOOT button, NVS force-portal flag), `radar_location`
  (lat/lon in NVS), `adsb_client` (HTTPS fetch + ArduinoJson parse into a fixed `Aircraft[64]`).
- **`ui/`** — `radar_geo` (the shared projection/clipping math), `radar_display` (grid + aircraft compositing),
  `runway_overlay`, `status_screens` (portal / connecting / reset screens), `radar_range` (range presets +
  units + runway toggle, all in NVS).
- **`data/`** — `large_airports_data.cpp` is **generated**; never hand-edit it or `include/data/large_airports.h`.
  1166 airports / 1706 runway segments as `int32` 1e-7-degree fixed point, ~2.9k lines, dominating flash use.

`main.cpp` owns the state machine: boot → optional setup screen → `wifiSetupConnect()` → radar; the loop polls
the BOOT button, services `wifiLoop()` (keeps the LAN portal alive), reconnects with a grace period on Wi-Fi
loss, and redraws every `kRenderIntervalMs`.

**ADS-B runs on its own FreeRTOS task** (`startFetchTask`), because a fetch blocks — almost all of it waiting
on the socket — and that froze the render loop for half of every cycle. The `WiFiClientSecure`/`HTTPClient`
pair is **file-scope and reused**: mbedTLS needs two ~16.4 KB contiguous blocks, and re-finding them every cycle in
a fragmented heap caused intermittent `SSL - Memory allocation failed` storms. Reusing the connection also
dropped the cycle from ~4.6 s to ~3.5 s by skipping the handshake. Any error path calls `s_client.stop()` so
the next attempt renegotiates. The task parses into a back
buffer and publishes it under a mutex; `drawAircraft()` takes that lock via `aircraftLock()` and *skips the
traffic layer* rather than stalling if it can't get it. Never call `wifiLoop()` / WiFiManager `process()` from
the fetch task — it is not thread-safe and `loop()` already owns it. `fetchTaskStackFree()` reports the task's
stack high-water mark and is logged every 32nd fetch (4,820 B of 8,192 used when last measured); watch it
after anything that deepens the fetch call path, since mbedTLS depth varies with the server's cert chain.

### Rendering model

`radarDisplayDraw()` and `radarDisplayRefreshAircraft()` both call `renderFrame()`, which composites the *whole*
frame (background, rings, crosshairs, runway overlay, center dot, labels, aircraft) into an off-screen
`LGFX_Sprite` and blits it in a single `pushSprite` — this is what kills flicker. Pixels are never cached
between frames (a second 240x240x16bpp sprite would need 115 KB and there is ~31 KB free, in a 9.2 KB largest block), but the runway
overlay caches its *screen-space geometry* in `runway_overlay.cpp` and rebuilds only when the range preset or
radar centre moves. Measured frame budget: **43.8 ms total (23 FPS ceiling)** = grid 10.5 + aircraft 21.5 + an
11.6 ms `pushSprite` (43.6 ms of accounted phases; 43.8 ms measured end to end), the last being a pure SPI transfer at 80 MHz (115,200 B x 8 / 80 MHz = 11.5 ms
theoretical). Rendering runs at `kRenderIntervalMs` (100 ms) independently of the ~3.5 s fetch cycle. The
sprite is 240×240×16bpp ≈ **115 KB**, on a
chip with ~320 KB heap, so any new large allocation must be checked against that. `ensureFrameSprite()` failing
falls back to drawing straight to the panel.

Drawing code writes through the `s_draw` pointer, swapped by the RAII `DrawScope` so the same helpers can target
either the sprite or the panel. `initPalette()` converts the RGB triples in `radar_theme.h` to RGB565 at runtime
and **swaps R/B for the aircraft color** when `kDisplayRgbOrder` is set (BGR panel).

Font sizing is measured, not hardcoded: `findVlwSizeForHeight()` binary-searches a VLW `setTextSize` scale to hit
a target cap height in px, and `pickGfxFontClosest()` picks the nearest bitmap font in the fallback path. Results
are cached in `s_*_metrics_ready` flags.

### Geometry — `ui/radar_geo.{h,cpp}`

All lat/lon → pixel math lives in **one** place: `offsetKmFromCenter`, `pxPerKm`, `latLonToScreen`,
`distSqFromCenter`, `clipPointToOuterRing`. Both `radar_display.cpp` and `runway_overlay.cpp` call it as
`radar::…`; do not re-inline copies (they were deduplicated for exactly that reason).

Equirectangular projection about the configured centre, north = screen up: `dy_km = Δlat × 111`,
`dx_km = Δlon × 111 × cos(centre lat)`. The `cos` factor is **required** — without it everything east–west is
stretched by `1/cos(lat)` (≈1.64× at 52°N). It is cached and recomputed only when the centre moves: the C3 has no hardware
FPU, and the runway cache rebuild calls it once per airport across the whole dataset.

Range presets label **ring 3 = ¾ of the outer radius**, so `outer_km = ring3_km × 4/3` (`radar_range.h`).
`fetchRadiusKm()` scales the outer range up to the screen edge (107 px grid vs 118 px rim) so aircraft outside
the outer ring still have data for the rim bearing dots.

### Persistence — three separate NVS namespaces

| Namespace | Owner | Keys |
|---|---|---|
| `planeradar` | `ui/radar_range.cpp` | `rangeIdx`, `useMiles`, `showRwys` |
| `radar` | `services/radar_location.cpp` | `lat`, `lon` |
| `wifi` | `services/wifi_setup.cpp` | `portal` (force-portal-on-next-boot flag) |
| — | WiFiManager/esp_wifi | SSID + password (not via `Preferences`) |

They are deliberately separate to avoid `Preferences` handle conflicts. A long BOOT press clears Wi-Fi
credentials, the location, and the unit/runway flags — but **not** `rangeIdx`, which survives a reset.

### Portal settings

Custom fields are `WiFiManagerParameter`s in `wifi_setup.cpp`, defaults refreshed by
`refreshPortalParamDefaults()` and persisted in `onPortalParamsSaved()`. Checkboxes are faked by injecting
`type="checkbox"[ checked]` into the parameter's HTML attribute string; an unchecked box submits nothing, which
`portalCheckboxChecked()` reads as false. The same `WiFiManager` instance serves the setup AP and the LAN portal
(`startWebPortal()`), so `wifiLoop()` must be called every iteration. (It used to be passed to a `setPollFn` hook so the
portal survived the blocking fetch; that hook is gone — the fetch runs on its own task now, and WiFiManager's
`process()` must never be called from it.)

## Non-negotiable: speed and memory come first

This is a 160 MHz single-core RISC-V part with **no FPU** and ~320 KB of heap, of which the frame
sprite alone takes 115 KB and mbedTLS permanently holds ~33 KB for the reused TLS session. Measured with the TLS session held: free heap at the start of a request is **30,856 B — bit-identical across
38 consecutive fetches**, and the **largest contiguous block is 9,204 B**. A fetch transiently consumes ~6 KB on device
(the filtered JSON document, in 4 KB pool chunks) and returns it; the same document measures ~9.6 KB peak
when parsed on a host with a tracking allocator against a captured 23-aircraft payload — the device figure is
lower simply because the local sky is quieter. mbedTLS permanently holds ~33 KB as *two*
~16.4 KB blocks plus a ~2.5 KB context; the frame sprite holds 115 KB; the fetch task's 8 KB stack and the
second aircraft buffer come out of the same pool.

**That 9,204 B largest block is the number to design against.** One existing consumer already brushes it:
WiFiManager assembles each portal page into a single contiguous `String`, and the `/wifi` scan page crosses
9.2 KB at roughly 16 visible access points — so in a dense-Wi-Fi area that page can fail to render while the
radar is running. `/param` is far smaller, and the setup portal (after a BOOT reset) runs with no TLS session
held. Nothing may request a larger contiguous
allocation at runtime. It is also why the fetch task releases the TLS session (`s_client.stop()`) the moment
the link drops: those ~33 KB must be back in the pool before WiFi restarts, or the reconnect itself can fail
for want of memory. Every review and every change
must be judged against that budget, not against what would be reasonable on a desktop.

Concretely, when writing or reviewing code here:

- **Fragmentation matters as much as totals.** A 16 KB allocation can fail with 31 KB free when the
  largest contiguous block is 9.2 KB — the measured state on this device. Prefer
  fixed-size buffers, streaming, and small chunked allocations over one large block. This is exactly
  what broke the ADS-B client (`payload.reserve(content_length + 1)`).
- **`kDisplaySpiWriteHz` is 80 MHz, which is out of spec for this pinout.** 80 MHz is the ESP32-C3's
  *IOMUX* ceiling; SCLK=GPIO4 and MOSI=GPIO3 are not the FSPI IOMUX pins (GPIO6/GPIO7), so this bus routes
  through the GPIO matrix, which is rated to 40 MHz. It is verified working on the author's unit and halves
  the 23 ms blit, but it has no margin and may not hold across other builds, wiring or temperature. Speckle,
  torn rows or colour corruption ⇒ drop to 40 MHz in `include/config.h` first.
- **No heap in the draw path.** Aircraft live in a static `Aircraft[64]`; the airport dataset is
  `const` in flash; the frame sprite is allocated once and reused. Keep it that way.
- **Watch per-frame cost.** The runway pass *used to* walk all 1,706 segments every redraw (~34 ms);
  it now draws at most 32 cached screen-space segments and only re-scans the dataset when the range
  preset or radar centre moves. Cache anything trigonometric that does not change per call --
  `radar_geo.cpp` caches `cos(centre latitude)` for the same reason.
- **Check return values of allocating calls.** `String::concat`, `reserve`, and `createSprite` all
  fail silently by returning false; ignoring that turns an allocation failure into a hang or corruption.
- **Measure, don't estimate.** ArduinoJson is header-only and compiles on the host: a tracking
  allocator against a real captured payload gives exact peaks. `ESP.getFreeHeap()` /
  `ESP.getMaxAllocHeap()` around a suspect block gives the on-device truth. Both were decisive here,
  and both times the device contradicted a plausible-sounding model.
- **Report the size delta** from the build output with any change that touches the hot path.

## Conventions

- C++17, 2-space indent, ~90 col. `kPascalCase` for constants, `s_` for file statics, `g_` for globals,
  anonymous namespaces for internals, namespaces `config`, `ui::radar`, `ui::runway`, `services::*`, `data::*`.
- All tunables (pins, timings, colors, defaults) live in `include/config.h` and `include/ui/radar_theme.h` —
  add new ones there rather than inline literals.
- `Serial.printf` at 115200 is the only debugging channel.

## Loose ends

The `spiffs` partition (896 KB) in `partitions/plane_radar.csv` is unused — the VLW font is embedded in the app
image via `board_build.embed_files`, not stored in SPIFFS. It is free space if an OTA slot is ever wanted.
`otadata` is present but there is only one app partition, so OTA is not wired up.
