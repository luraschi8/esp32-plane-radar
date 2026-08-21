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

There is **no test suite, linter, or formatter** configured. Verification = a clean build with no `src/`-or-
`include/` warnings + flash/RAM fit in the size report + the on-hardware checklist. **`OPS.md` is the full
build / verify / flash / troubleshooting reference — read it before doing any of those.** Current baseline:
RAM 15.6% (51028 B static), Flash 39.5% (1243500 B of 3 MB).

Do not reintroduce a `namespace fonts = lgfx::v1::fonts;` alias in any file: LovyanGFX >= 1.2.x already declares
a global `namespace fonts` plus `using namespace fonts;` in `lgfx_fonts.hpp`, so the alias is a redeclaration
error and `fonts::FreeSansBold12pt7b` etc. resolve without it. (This broke the build on `^1.2.7` -> 1.2.27.)

## Architecture

Single-threaded Arduino `setup()`/`loop()` — no RTOS tasks, no dynamic allocation in the hot path.
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
loss, and every `kAdsbFetchIntervalMs` fetches + redraws.

### Rendering model

`radarDisplayDraw()` and `radarDisplayRefreshAircraft()` both call `renderFrame()`, which composites the *whole*
frame (background, rings, crosshairs, runway overlay, center dot, labels, aircraft) into an off-screen
`LGFX_Sprite` and blits it in a single `pushSprite` — this is what kills flicker. Pixels are never cached
between frames (a second 240x240x16bpp sprite would need 115 KB and there is ~35-42 KB free), but the runway
overlay caches its *screen-space geometry* in `runway_overlay.cpp` and rebuilds only when the range preset or
radar centre moves. Measured frame budget: 55.3 ms total (18 FPS ceiling) = grid 10.5 + aircraft 21.5 + a
23.1 ms `pushSprite`, the last being the SPI transfer at its 40 MHz limit (115,200 B x 8 / 40 MHz = 23.0 ms). The sprite is 240×240×16bpp ≈ **115 KB**, on a
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
stretched by `1/cos(lat)` (≈1.64× at 52°N). It is cached and recomputed only when the centre moves, since the
runway pass calls it ~1700×/frame and the C3 has no hardware FPU.

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

They are deliberately separate to avoid `Preferences` handle conflicts; a long BOOT press clears all of them.

### Portal settings

Custom fields are `WiFiManagerParameter`s in `wifi_setup.cpp`, defaults refreshed by
`refreshPortalParamDefaults()` and persisted in `onPortalParamsSaved()`. Checkboxes are faked by injecting
`type="checkbox"[ checked]` into the parameter's HTML attribute string; an unchecked box submits nothing, which
`portalCheckboxChecked()` reads as false. The same `WiFiManager` instance serves the setup AP and the LAN portal
(`startWebPortal()`), so `wifiLoop()` must be called every iteration and is also passed to
`services::adsb::setPollFn()` so the portal stays responsive during blocking HTTP reads.

## Non-negotiable: speed and memory come first

This is a 160 MHz single-core RISC-V part with **no FPU** and ~320 KB of heap, of which the frame
sprite alone takes 115 KB and mbedTLS takes ~32 KB per request. Measured free heap during an ADS-B
fetch is **~35-42 KB, with a largest contiguous block of only 9-20 KB**. Every review and every change
must be judged against that budget, not against what would be reasonable on a desktop.

Concretely, when writing or reviewing code here:

- **Fragmentation matters as much as totals.** A 16 KB allocation can fail with 37 KB free. Prefer
  fixed-size buffers, streaming, and small chunked allocations over one large block. This is exactly
  what broke the ADS-B client (`payload.reserve(content_length + 1)`).
- **No heap in the draw path.** Aircraft live in a static `Aircraft[64]`; the airport dataset is
  `const` in flash; the frame sprite is allocated once and reused. Keep it that way.
- **Watch per-frame cost.** `renderFrame()` walks all 1,706 runway segments every redraw, and the
  runway pass calls the projection ~1,700x. Cache anything trigonometric that does not change per
  call -- `radar_geo.cpp` caches `cos(centre latitude)` for this reason.
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
