# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ESP32-C3 firmware for a live ADS-B radar displayed on a 1.28" round GC9A01 (240x240) screen. Aircraft positions are fetched from `opendata.adsb.fi` and rendered on a sonar-style circular grid. First-time WiFi configuration is handled via a captive portal (WiFiManager). All user-configurable values (location, range, units, runway toggle) persist in NVS flash.

## Build and flash

PlatformIO project, single environment `supermini`. C++17 (`-std=gnu++17`).

```bash
pio run                    # build
pio run -t upload          # build + flash over USB
pio device monitor         # serial monitor (115200 baud)
```

Merged web-flash image (single .bin at 0x0 for esptool-js):

```bash
pio run -t merge -e supermini   # output: .pio/build/supermini/firmware-merged.bin
# or:
./scripts/merge-firmware.sh     # output: release/plane-radar-merged.bin
```

There are no unit tests. Verification is done by flashing and observing behavior on the device.

## Releasing

Tag with `v*` and push. The `release.yml` GitHub Action builds and attaches the merged binary to a GitHub Release.

```bash
git tag v1.0.0
git push origin v1.0.0
```

## Architecture

### Layer structure

The firmware has four layers. Dependencies flow downward only.

1. **`main.cpp`** -- Arduino `setup()`/`loop()` entry point. Orchestrates WiFi connection, BOOT button handling, ADS-B polling, and display refresh. No business logic lives here.

2. **`services/`** -- External I/O and persistent state.
   - `wifi_setup` -- WiFiManager integration, captive portal, mDNS, BOOT button ISR and long-press/tap handling. Owns the WiFiManager instance and portal custom parameters (lat, lon, miles, runways). Also manages the "force portal" NVS flag for credential resets.
   - `adsb_client` -- HTTPS client for `opendata.adsb.fi/api/v3/`. Parses JSON into a fixed-size `Aircraft[64]` array. Uses a poll callback (`PollFn`) to keep WiFiManager responsive during HTTP I/O.
   - `radar_location` -- Reads/writes radar center lat/lon to NVS (`radar` namespace).

3. **`ui/`** -- Display rendering, no I/O.
   - `radar_display` -- Main render loop. Double-buffered via an LGFX_Sprite (falls back to direct panel draw if sprite allocation fails). Draws grid, cardinals, scale label, runway overlay, and aircraft symbols/tags in a single `pushSprite`.
   - `radar_range` -- Range preset state (5/10/15/25 km), miles/km toggle, runway toggle. Persists to NVS (`planeradar` namespace).
   - `radar_theme` -- All layout constants (radii, font sizes, margins) and RGB palette targets as `constexpr`. Colors are converted to RGB565 at runtime in `initPalette()` with an R/B swap for BGR panels.
   - `runway_overlay` -- Renders major-airport runways from embedded data. Clips lines and labels to the outer ring.
   - `status_screens` -- WiFi setup/connecting/error screens.

4. **`hardware/`** -- Display driver and font loading.
   - `display` -- LovyanGFX init, global `tft` instance.
   - `lgfx_config.hpp` -- SPI bus and GC9A01 panel configuration (pins from `config.h`).
   - `display_font` -- VLW smooth font loader (embedded via `board_build.embed_files`).

### Data

- `data/large_airports.h` / `large_airports_data.cpp` -- Embedded airport/runway dataset (OurAirports `large_airport` category). Coordinates stored as `int32_t` E7 format. Regenerate with `python3 scripts/build_large_airports.py`.

### Key design details

- **Single-threaded**: everything runs in the Arduino `loop()` on core 0. The BOOT button uses an ISR (`IRAM_ATTR`) that sets flags consumed by polling in `loop()`.
- **Poll callback**: `adsb_client` accepts a `PollFn` (set to `wifiLoop`) so the WiFiManager web portal stays responsive during HTTP requests.
- **NVS namespaces**: `"wifi"` for force-portal flag, `"radar"` for lat/lon, `"planeradar"` for range index / miles / runways.
- **GC9A01 BGR quirk**: the panel uses BGR subpixel order. `initPalette()` swaps R and B channels in `color565()` calls when `kDisplayRgbOrder` is true, so logical red appears red on screen.
- **Partition layout**: custom 4 MB partition table (`partitions/plane_radar.csv`) with a single 3 MB app slot (no OTA), 896 KB SPIFFS, and a coredump partition.
- **WiFi TX power**: set to 8.5 dBm in both AP and STA modes to stay within safe power limits for the board.

## Configuration

All hardware pins, timing constants, WiFi behavior, and default coordinates are in `include/config.h`. Radar layout and color palette are in `include/ui/radar_theme.h`. Range presets are in `include/ui/radar_range.h`.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) ^1.2.7 -- display driver
- [WiFiManager](https://github.com/tzapu/WiFiManager) ^2.0.17 -- captive portal
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) ^7.4.2 -- ADS-B JSON parsing
