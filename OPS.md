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

There is one build environment: **`supermini`** (`platformio.ini`). `-e supermini` is optional but explicit.

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
pio run -e supermini -t clean     # drop object files, keep downloaded packages
pio run -e supermini -t merge     # produce firmware-merged.bin (requires a prior build)
rm -rf .pio                       # nuclear: also re-downloads toolchain + libs
```

---

## 3. Verify

There is **no automated test suite** — no unit tests, no host-side harness, no linter, no formatter.
Verification is: it compiles cleanly, it fits, and it behaves on hardware. Do all three.

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
RAM:   [==        ]  15.6% (used 51028 bytes from 327680 bytes)
Flash: [====      ]  39.5% (used 1243500 bytes from 3145728 bytes)
```

Read the RAM number as *static* usage only. At runtime the radar allocates a **240x240x16bpp sprite
(~115 KB)** for the double-buffered frame, plus WiFi/TLS buffers for each ADS-B fetch. If RAM
statics climb far above the baseline, `ensureFrameSprite()` starts failing and the display falls back
to flicker-prone direct drawing — the serial log prints `radar: frame sprite alloc failed`.

Flash growth is dominated by `src/data/large_airports_data.cpp`. The app partition is 3 MB.

### 3.3 Behaves on hardware

Flash (section 4), open the serial monitor, and walk the checklist:

| Step | Expected |
|------|----------|
| Power on | `Plane Radar` banner on serial; display lights up |
| No saved Wi-Fi | Yellow setup screen, AP `PlaneRadar-Setup` appears in the Wi-Fi list |
| Portal at `http://plane-radar.local` or `http://192.168.4.1` | Wi-Fi form + Latitude / Longitude / miles / runways fields |
| After saving Wi-Fi | `Connected: <ssid>  IP <addr>`, then the radar grid draws |
| Every ~3 s | `adsb: N aircraft` on serial; symbols move between frames |
| Short-tap BOOT | `Range: 10km (outer ~13 km)` — cycles 5 → 10 → 15 → 20 → 25 km, ring label changes |
| Hold BOOT 3 s | `BOOT held — resetting WiFi`, reset screen, reboot into the portal |
| Reconnect (`http://<device-ip>`) | Portal reachable while the radar keeps running |

**Sanity-check the geometry** when you touch the projection: set your real latitude/longitude in the
portal and confirm that a nearby airport's runways land in the right place and orientation, and that
aircraft east and west of you are not stretched outward. The projection scales longitude by
`cos(centre latitude)`; without it, everything east-west drifts outward by `1/cos(lat)` — about 1.64x
at 52 deg N. `src/ui/radar_geo.cpp` is the single place that math lives.

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
chmod +x scripts/merge-firmware.sh    # once
./scripts/merge-firmware.sh           # build + merge -> release/plane-radar-merged.bin
./scripts/merge-firmware.sh --no-build   # merge an existing build only
```

Then open [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge, put the board in
download mode (4.2), connect, and flash the single `.bin` at offset **`0x0`** — chip **ESP32-C3**,
**4 MB** flash. Offset `0x0` is required: the merged image contains the bootloader.

Prebuilt images are on the [Releases page](https://github.com/MatixYo/ESP32-Plane-Radar/releases).

### 4.4 Erase flash

Wipes the app *and* all NVS (Wi-Fi credentials, location, range, units). A 3-second BOOT hold does the
same thing from the app and is usually what you want instead.

```bash
pio run -e supermini -t erase
```

---

## 5. CI and releases

| Workflow | Trigger | Output |
|----------|---------|--------|
| `.github/workflows/build.yml` | push / PR to `main` | artifact `plane-radar-supermini` (merged + split `.bin`, ~90 days) |
| `.github/workflows/release.yml` | tag `v*` | GitHub Release with `plane-radar-<tag>.bin` + `.sha256` |

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
the flash figure — the dataset is the largest single contributor to image size.

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
| `radar: frame sprite alloc failed` | Out of heap for the 115 KB frame buffer; display falls back to direct drawing |
| `adsb: HTTP -1` / `adsb: empty response` | Transient network or adsb.fi hiccup; the previous frame is kept. Persistent failures: check DNS and that `kAdsbFetchIntervalMs` (3 s) still respects adsb.fi's 1 req/s limit |
| Portal saves but nothing changes | `Invalid lat/lon in portal — keeping previous location` on serial means the coordinates failed parsing or range validation |
| `.local` address won't resolve | mDNS is slow or blocked on some clients; use the IP printed on serial at boot |
