# OPS — build, verify, flash

Everything needed to get firmware from this repo onto an **ESP32-C3 Super Mini**.
For what the firmware *does*, see [README.md](README.md); for code structure, see [CLAUDE.md](CLAUDE.md).

---

## 1. Prerequisites

| Need | Notes |
|------|-------|
| PlatformIO Core | `pip install --upgrade platformio`, or the VS Code **PlatformIO IDE** extension |
| Python 3 | Only for `scripts/build_large_airports.py` (dataset regeneration) |
| USB-C data cable | Charge-only cables are the single most common "board not detected" cause |

**`pio` is often not on `PATH`.** The VS Code extension installs it into its own virtualenv:

```bash
~/.platformio/penv/bin/pio --version          # macOS / Linux
export PATH="$HOME/.platformio/penv/bin:$PATH" # optional, for the current shell
```

`scripts/merge-firmware.sh` already falls back to that path automatically. Every `pio` command below
works with either form.

There are three environments: **`supermini`** (the firmware), **`supermini-debug`** (the same firmware with
verbose logging compiled in — section 3.4) and **`native`** (host unit tests, section 3.0).
`default_envs = supermini` makes a bare `pio run` build the firmware only; `-e supermini` is optional but
explicit. The test env is always selected deliberately with `-e native`.

---

## 2. Build

```bash
pio run -e supermini
```

First run downloads the toolchain and libraries into `.pio/` (a few minutes); later builds take seconds.

Artifacts land in `.pio/build/supermini/`:

| File | Flash offset | What it is |
|------|--------------|------------|
| `firmware.bin` | `0x10000` | The application |
| `bootloader.bin` | `0x0` | Second-stage bootloader |
| `partitions.bin` | `0x8000` | Partition table (`partitions/plane_radar.csv`) |
| `firmware-merged.bin` | `0x0` | All of the above + `boot_app0`, one file (built by `-t merge`) |

Useful variants:

```bash
pio run                           # same as -e supermini (default_envs)
pio run -e supermini-debug        # identical firmware + verbose logging (section 3.4)
pio run -e supermini -t clean     # drop object files, keep downloaded packages
pio run -e supermini -t merge     # produce firmware-merged.bin (builds the app first if needed)
rm -rf .pio                       # nuclear: also re-downloads toolchain + libs
```

---

## 3. Verify

There is a **host-side unit test suite** (section 3.0) plus the on-hardware checks below. There is still no
linter or formatter. Verification is: tests pass, it compiles cleanly, it fits, and it behaves on hardware.

### 3.0 Unit tests

```bash
pio test -e native                 # whole suite (211 tests, 9 suites) in ~18 s, ASan+UBSan on
pio test -e native -f test_geo     # one suite
```

These build for the **host**, not the device: `test/` is never compiled by `pio run`, so nothing here reaches
the firmware image (verified by putting a `#error` in `test/` and watching `pio run -e supermini` still
succeed). The shipped `.cpp` files are included directly and compiled against mocks in `test/mocks/`
(Arduino/GPIO, Preferences/NVS, WiFi, HTTPClient, a scriptable TLS client, FreeRTOS, WiFiManager, ESPmDNS,
esp_wifi, a `display_font` stub, and a **recording-canvas LovyanGFX** that logs every draw call), so the tests
exercise real code rather than a reimplementation of it.

Note `[env:native]` puts `-I test/mocks` *ahead of* `-I include`, so a header under `test/mocks/` mirroring an
`include/` path silently replaces the real one in host builds. Today only `hardware/display_font.h` does this.
If you add another, check its signatures still match the header it shadows.

Fixtures in `test/fixtures_*.h` are **real adsb.fi responses** captured from the device's own location; the
geometry suite checks our projection against the API's own `dst`/`dir` fields, which are independent ground
truth.

### 3.1 Compiles cleanly

A clean build must finish `[SUCCESS]` with **zero warnings from `src/` or `include/`**
(warnings from `framework-arduinoespressif32` and the libraries are pre-existing and expected):

```bash
pio run -e supermini -t clean && pio run -e supermini 2>&1 \
  | grep -E "^(src|include)/.*(warning|error)"     # must print nothing
```

### 3.2 Fits

The size report at the end of every build is the memory budget. Current baseline:

```
RAM:   [==        ]  16.8% (used 55012 bytes from 327680 bytes)
Flash: [====      ]  39.7% (used 1247850 bytes from 3145728 bytes)
```

Read the RAM number as *static* usage only. At runtime the radar allocates a **240x240x16bpp sprite
(~115 KB)** for the double-buffered frame, plus WiFi/TLS buffers for each ADS-B fetch. If RAM
statics climb far above the baseline, `ensureFrameSprite()` starts failing and the display falls back
to flicker-prone direct drawing — the serial log prints `radar: frame sprite alloc failed`.

`src/data/large_airports_data.cpp` is the largest project-owned contributor to flash — `kRunways`
40,944 B + `kAirports` 18,656 B = 59.6 KB, 4.8% of the image — but the framework (lwIP, mbedTLS,
net80211) dominates the total. The app partition is 3 MB.

### 3.3 Behaves on hardware

Flash (section 4), open the serial monitor, and walk the checklist.

**Three `[E]`-level lines at every boot are expected and are not faults.** They come from the framework, not
from this project, and a healthy boot looks like this:

```
Plane Radar
[   735][E][esp32-hal-spi.c:227] spiAttachMISO(): SPI Does not have default pins on ESP32C3!
[   930][E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND
[   933][E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND
Connecting to WiFi (portal opens if needed)...
Connected: <ssid>  IP 192.168.1.96
LAN config: http://plane-radar.local or http://192.168.1.96
adsb: 7 aircraft (task stack free 3636 B)
```

- `spiAttachMISO` — the display is write-only, so `pin_miso = -1` in `lgfx_config.hpp`. Nothing to fix.
- Both `nvs_open failed: NOT_FOUND` lines are the **`wifi` namespace**, read twice at boot for the
  force-portal flag. That namespace is only ever written by a BOOT reset, so on a device that has never been
  reset it does not exist. Confirmed with the debug build, which prints
  `dbg: wifi: nvs namespace 'wifi' absent -- no pending force-portal flag` next to each one. If you see a
  *third* such line, that one is real: it means `radar` or `planeradar` is missing and the location or the
  range preset has been lost.

Then the checklist:

| Step | Expected |
|------|----------|
| Power on | `Plane Radar` banner on serial; display lights up |
| No saved Wi-Fi | Yellow setup screen, AP `PlaneRadar-Setup` appears in the Wi-Fi list |
| Portal at `http://plane-radar.local` or `http://192.168.4.1` | Wi-Fi form + Latitude / Longitude / miles / runways fields |
| After saving Wi-Fi | `Connected: <ssid>  IP <addr>`, then the radar grid draws |
| Every ~3.5 s | `adsb: N aircraft` on serial (a 3 s gap after a ~0.5 s fetch); every 32nd line also reports fetch-task stack headroom |
| Short-tap BOOT | `Range: 10km (outer ~13 km)` — cycles 5 → 10 → 15 → 20 → 25 km, ring label changes. Note a long press does **not** reset the range |
| Hold BOOT 3 s | `BOOT held — resetting WiFi`, reset screen, reboot into the portal |
| Reconnect (`http://<device-ip>`) | Portal reachable while the radar keeps running |
| Save a setting with an empty sky (toggle runways, or km↔mi) | The panel repaints **immediately**. The renderer idles when there is no traffic, so a settings change has to request its own frame — this regressed once and was invisible whenever aircraft happened to be up |

**Sanity-check the geometry** when you touch the projection: set your real latitude/longitude in the
portal and confirm that a nearby airport's runways land in the right place and orientation, and that
aircraft east and west of you are not stretched outward. The projection scales longitude by
`cos(centre latitude)`; without it, everything east-west drifts outward by `1/cos(lat)` — about 1.64x
at 52 deg N. `src/ui/radar_geo.cpp` is the single place that math lives.

---

### 3.4 Diagnosing on real hardware — the debug build

`include/debug_log.h` adds verbose logging that is **compiled out of every normal build**. Turn it on by
flashing the debug environment; the sources are identical, only `-D PLANE_RADAR_DEBUG=1` differs:

```bash
pio run -e supermini-debug -t upload && pio device monitor
```

Every line is prefixed `dbg: ` so it is easy to filter. What you get:

| Line | Use it for |
|------|-----------|
| `dbg: heap <stage> free N largest N` | at boot, after the display, after the 115 KB sprite, after setup, and around every fetch — this is how the memory table in CLAUDE.md was measured |
| `dbg: location: …` / `dbg: range: …` | what the device actually loaded from NVS, and whether each namespace existed at all |
| `dbg: wifi: force-portal flag …` | whether a BOOT reset is pending |
| `dbg: heap before fetch …` | the heap **at the moment mbedTLS asks for its two ~16.4 KB blocks** — the only reading that explains a TLS allocation failure. A `before fetch` with no matching `fetch:` line means the task stalled |
| `dbg: heap begin failed / http failed / no stream` | the failure paths, each with a heap reading, so a fragmentation failure is distinguishable from a network one |
| `dbg: fetch: N kept, N ms, stack free N B` | fetch cost and fetch-task stack headroom, every cycle instead of every 32nd |
| `dbg: frame: grid N us + traffic N us + blit N us` | the frame budget, throttled to one line per second so the logging does not distort what it measures |

Flash `-e supermini` again afterwards. Verify the release image really is clean:

```bash
pio run -e supermini && strings .pio/build/supermini/firmware.bin | grep -c "dbg: "   # must print 0
```

When adding a call site: never inside the per-aircraft draw loop, the per-segment runway loop, or the fetch
task's inner loop. `Serial` is USB CDC here, not a UART, so the hazard is not baud rate — `HWCDC::write`
**blocks for up to 100 ms** when no host is draining the TX ring, which is a whole render tick against a
~35 ms frame. In those paths keep each rendered line under 64 characters (`Print::printf` uses a 64-byte
stack buffer and `malloc`s past it). Never put a side effect in a `DEBUG_LOG` argument — arguments are not
evaluated when logging is compiled out — and mark any local computed only for a message `[[maybe_unused]]`.

Format strings in `src/` are compiled with `-Wformat -Werror=format` (plus `-Wall -Wextra`), which required
`build_unflags = -Wno-format`: the framework disables format checking and project flags are emitted before
its own, so a bare `-Wformat` does nothing. Before that, passing an integer to `%s` in the fetch task — a
crash on device — compiled clean.

---

## 4. Upload to the ESP32

### 4.1 Normal path — USB

```bash
pio run -e supermini -t upload
pio device monitor                      # 115200 baud, set in platformio.ini
pio run -e supermini -t upload -t monitor   # both in one go
```

The Super Mini uses **native USB-CDC** (`ARDUINO_USB_CDC_ON_BOOT=1`), so the serial port is the chip
itself, not a USB-UART bridge. Consequences worth knowing:

- The port **disappears and re-enumerates** on reset and on flash. That is normal.
- Serial output before `Serial.begin()` settles is lost; `setup()` has a `delay(500)` for this reason.
- If the sketch crashes hard or is mid-boot, the port can vanish — use download mode.

Pick the port explicitly if auto-detection guesses wrong:

```bash
pio device list                                  # macOS: /dev/cu.usbmodem*  Linux: /dev/ttyACM*
pio run -e supermini -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX
```

Exit the monitor with **Ctrl-C** (or `Ctrl-]`).

### 4.2 Download (bootloader) mode

Needed when the board won't accept an upload, has no port, or is running a firmware that crash-loops:

1. Hold **BOOT**
2. Tap **RESET** (or replug USB) while still holding BOOT
3. Release BOOT

The board is now in ROM download mode and will stay there until reset. Upload normally.
Note this is the same BOOT button the firmware uses for range/reset — its behaviour depends entirely
on *when* it is pressed (at power-on = bootloader, while running = app control).

### 4.3 Browser flashing (no toolchain)

For handing a build to someone else, or reflashing without PlatformIO:

```bash
./scripts/merge-firmware.sh           # build + merge -> release/plane-radar-merged.bin
./scripts/merge-firmware.sh --no-build   # merge an existing build only
```

Then open [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge, put the board in
download mode (4.2), connect, and flash the single `.bin` at offset **`0x0`** — chip **ESP32-C3**,
**4 MB** flash. Offset `0x0` is required: the merged image contains the bootloader.

Prebuilt images are on the [Releases page](https://github.com/MatixYo/ESP32-Plane-Radar/releases).

### 4.4 Erase flash

Wipes the app *and* all NVS — Wi-Fi credentials, location, range preset, units and the runway toggle.
Note a 3-second BOOT hold is **not** equivalent: it clears credentials, location, units and the runway
toggle, but the saved range preset survives.

```bash
pio run -e supermini -t erase
```

---

## 5. CI and releases

| Workflow | Trigger | Output |
|----------|---------|--------|
| `.github/workflows/build.yml` | push to `main`/`master`, any PR, manual dispatch; runs `pio test -e native`, then builds both `supermini` and `supermini-debug` | artifact `plane-radar-supermini` (merged + split `.bin`, ~90 days) |
| `.github/workflows/release.yml` | tag `v*`, or manual dispatch (falls back to `manual-<sha7>`) | GitHub Release with `plane-radar-<tag>.bin` + `.sha256` |

```bash
git tag v1.0.0 && git push origin v1.0.0
```

Both workflows run the same `pio run -e supermini` + `pio run -t merge -e supermini` as local builds,
so a green local build is a good predictor of green CI.

---

## 6. Regenerating the airport dataset

```bash
python3 scripts/build_large_airports.py
```

Downloads `airports.csv` and `runways.csv` from
[OurAirports](https://github.com/davidmegginson/ourairports-data), keeps `large_airport` entries with
open, non-helipad runways, and **overwrites** `include/data/large_airports.h` and
`src/data/large_airports_data.cpp`. Never hand-edit those two files. Rebuild afterwards and re-check
the flash figure — the dataset is the largest project-owned contributor to image size (59.6 KB today).

---

## 7. Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `'namespace fonts = ...' conflicts with a previous declaration` | LovyanGFX ≥ 1.2.x declares a global `fonts` namespace. The project's own aliases were removed to fix this — if it reappears, delete the `namespace fonts = lgfx::v1::fonts;` line from the offending file |
| No serial port / upload fails | Data-capable cable? Then download mode (4.2). Native USB-CDC ports vanish on reset — that alone is not a fault |
| `A fatal error occurred: Failed to connect` | Board not in download mode, or another program (a serial monitor) holds the port |
| Blank / white display | Check SPI wiring against the README table; `kDisplayInvert` and `kDisplayRgbOrder` in `include/config.h` vary between GC9A01 modules |
| Colours inverted or red/blue swapped | Flip `kDisplayInvert` / `kDisplayRgbOrder`. `initPalette()` swaps R/B for the aircraft colour when `kDisplayRgbOrder` is set |
| Reboot loop when Wi-Fi connects | Brownout — the Super Mini's regulator is marginal at full TX power. TX power is deliberately capped at `WIFI_POWER_8_5dBm` in both AP and STA paths; do not raise it. Also try a better USB supply |
| `radar: frame sprite alloc failed` | Out of heap for the 115 KB frame buffer; display falls back to direct drawing. Flash `-e supermini-debug` to see the heap ladder at boot and find what took the space |
| `[E] spiAttachMISO(): SPI Does not have default pins` at boot | Expected — the display bus is write-only (`pin_miso = -1`). Not a fault |
| `[E] nvs_open failed: NOT_FOUND` twice at boot | Expected — the `wifi` namespace only exists after a BOOT reset (section 3.3). A **third** occurrence means the location or range preset was lost |
| `adsb: HTTP -1`, `adsb: HTTP -11`, `adsb: no response stream` | Transient network or adsb.fi hiccup; the previous frame is kept and the fetch task retries after the next gap. After 60 s with no successful fetch the traffic layer is cleared rather than shown stale |
| `adsb: JSON parse error: IncompleteInput (heap=… largest=…)` | The response was cut off mid-body — almost always the server closing the reused keep-alive connection between requests, which is the trade-off for skipping the TLS handshake each cycle. **Check the heap figures in the message before assuming memory:** healthy is ~22–28 KB free with a ~9 KB largest block, and if those look normal it was the connection, not RAM. The next cycle renegotiates. Observed roughly once every 2–3 minutes on a domestic connection; one lost cycle is invisible on screen because the previous traffic keeps dead-reckoning |
| Portal saves but nothing changes | `Invalid lat/lon in portal — keeping previous location` on serial means the coordinates failed parsing or range validation |
| `.local` address won't resolve | mDNS is slow or blocked on some clients; use the IP printed on serial at boot |
| Config portal serves a **blank page** (usually `/wifi` in a dense-Wi-Fi area) | WiFiManager builds each page into one contiguous `String` (~4.9 KB fixed plus ~278 B per scanned AP, so ~9.2 KB at ~16 APs), and while the radar is running the reused TLS session pins ~33 KB, leaving a largest free block of ~9.2 KB. Use `/param` (location, units, runways — a much smaller page), or hold BOOT 3 s to reboot into the setup portal, where no TLS session exists and the full heap is available |
