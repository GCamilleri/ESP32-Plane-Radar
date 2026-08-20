# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ESP32-C3 firmware for a live ADS-B radar displayed on a 1.28" round GC9A01 (240x240) screen. Aircraft positions are fetched from `opendata.adsb.fi` and rendered on a sonar-style circular grid. First-time WiFi configuration is handled via a captive portal (WiFiManager). Everything the on-device menu can change persists in NVS flash.

## Build and flash

PlatformIO project, C++17 (`-std=gnu++17`), three environments: `supermini` (the
default board build), `local` (same build plus a feed server URL from the
environment) and `native` (host unit tests).

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

Host unit tests cover the pure-logic modules (`pio test -e native`, no hardware),
and the airport generator has its own Python tests
(`pytest scripts/test_build_airports.py`). CI runs both. Everything display- or
radio-coupled is verified by flashing and observing behaviour on the device.

The `native` env compiles only the sources named in its `build_src_filter`
(currently `adsb_parse`, `adsb_feed`, `aircraft_motion`, `radar_geo`) plus header-only code such as
`ui/text_fit.h` and `data/military_ranges.h`. A new host-tested `.cpp` has to be
added to that filter or the test will fail to link, and anything it pulls in from a
hardware-coupled module needs a stub in `test/test_native/stubs.cpp`.

Building the `local` env without `RADAR_FEED_URL` set produces a firmware whose
feed URL is the empty string, which silently disables the server and social tags.
That is the same output as `supermini`, so a build that "works" but shows no tags is
usually a missing env var.

The feed server in `server/` has its own toolchain: `npm install` (types only, no
runtime dependencies), `npx tsc --noEmit` to typecheck, and `node src/index.ts` to
run it against the real adsb.fi. Needs Node 24+.

Note that Node runs those `.ts` files by stripping types rather than compiling.
Parameter properties and enums are valid TypeScript that this mode cannot express,
and they fail at *startup* rather than typecheck, which is why CI boots the server
as well as typechecking it.

## Releasing

Tag with `v*` and push. The `release.yml` GitHub Action builds and attaches two
binaries to a GitHub Release, and publishes the browser installer to GitHub Pages.

```bash
git tag v1.0.0
git push origin v1.0.0
```

Two images, because a settings-preserving update needs a different offset from a
fresh install: `plane-radar-<tag>.bin` is the merged image flashed at `0x0` (wipes
NVS, so it resets WiFi and preferences), and `plane-radar-app-<tag>.bin` is the app
partition alone flashed at `0x10000` (leaves NVS intact). The Pages installer
publishes both as separate ESP Web Tools manifests per version, `manifest-full.json`
and `manifest-keep.json`. Changing the partition table invalidates the app-only path
for existing devices, so that release has to say "full reflash required".

Releases are built from the `local` env with `RADAR_FEED_URL` set in the workflow, so
a web-flashed radar has working social tags with no configuration. A plain
`pio run` (env `supermini`) leaves the URL empty and produces a firmware with no feed
server and tags disabled, which is what building from source should give. If the feed
host ever changes, devices already flashed from a release keep pointing at the old one
and can only be moved by reflashing.

## Architecture

### Layer structure

The firmware has four layers. Dependencies flow downward only.

1. **`main.cpp`** -- Arduino `setup()`/`loop()` entry point. Orchestrates WiFi connection, BOOT button handling, ADS-B polling, and display refresh. No business logic lives here. It also arbitrates the button: the menu and the target picker each consume the button while open, and both disarm the 3s hold-to-reset (`bootButtonSetLongPressEnabled(false)`) because a hold already means something else on those screens.

2. **`services/`** -- External I/O and persistent state.
   - `wifi_setup` -- WiFiManager integration, captive portal, mDNS, BOOT button ISR and long-press/tap handling. Owns the WiFiManager instance and portal custom parameters (lat, lon, miles, runways). Also manages the "force portal" NVS flag for credential resets. The "LAN Cfg" menu toggle starts WiFiManager's `startWebPortal()`, an HTTP server on the STA address with no soft AP, so the radar never beacons once setup is done; `ensureSoftApOff()` runs after every STA connect because `shutdownConfigPortal()` closes the setup AP with `softAPdisconnect(false)`, which leaves the interface up.
   - `adsb_client` -- HTTPS client for the feed, and owner of the background fetch task. Prefers the `server/` feed when `config::kFeedProxyBaseUrl` is set, falling back to `opendata.adsb.fi/api/v3/` after repeated failures. Fills a fixed-size `Aircraft[64]` array. Uses a poll callback (`PollFn`) to keep WiFiManager responsive during HTTP I/O, though the callback is nulled while the fetch runs on its own task.
   - `adsb_feed` -- Pure parser for the server's line-oriented PR1 format, and the ICAO join that attaches social tags to aircraft. Host-testable, no I/O.
   - `adsb_parse` -- Pure JSON field parsing for the direct adsb.fi fallback path. Host-testable.
   - `social_tags` -- Device identity (NVS `social` namespace), handle, HMAC signing, and the single-slot claim/release queue. Owns no socket: `adsb_client` drains the queue on the connection the feed just used.
   - `radar_location` -- Reads/writes radar center lat/lon to NVS (`radar` namespace).

3. **`ui/`** -- Display rendering, no I/O.
   - `radar_display` -- Main render loop. Double-buffered via an LGFX_Sprite (falls back to direct panel draw if sprite allocation fails). Draws grid, cardinals, scale label, runway overlay, and aircraft symbols/tags in a single `pushSprite`.
   - `menu` -- The settings menu, opened by a 1s hold. Nine settings (Range, Heading, Labels, Airports, Poll Rate, Military, Sweep, LAN Cfg, Tags) plus two actions that fire on hold rather than cycling a value (Clear Tags, Reset WiFi). Tap moves, hold selects or goes back, and it closes after `kMenuTimeoutMs`. LAN Cfg has its own screen layout because it is the one setting whose "On" state is useless without an address to read off the display.
   - `radar_range` -- All persisted view state, not just range: preset index (5/10/15/25 km), miles/km, cumulative airport tier, heading rotation in 15 degree steps, label mode, poll rate (including whether motion is smoothed), sweep, military highlight, LAN portal flag, tags flag. Persists to NVS (`planeradar` namespace). Adding a setting means a key here and an entry in `menu.cpp`.
   - `aircraft_motion` -- Dead reckoning between polls. Holds a 64-slot table keyed by ICAO, extrapolates each aircraft along its own track, and eases the drawn position onto that so a correction arrives as a slide rather than a jump. Fed from `main.cpp` when a fetch completes, stepped once per frame by `radar_display`. Host-testable: it takes the current `millis()` as an argument rather than reading the clock.
   - `radar_geo` -- Shared geo and projection maths (offset from center, lat/lon to screen, ring clipping). Host-testable, and the reason `radar_display` and `runway_overlay` no longer carry duplicate copies.
   - `text_fit` -- Header-only UTF-8 aware ellipsizing. Truncating on a byte boundary tears multi-byte glyphs and the VLW font renders the remains as junk, so anything shortening a callsign or handle should go through `buildEllipsized()`.
   - `radar_theme` -- All layout constants (radii, font sizes, margins) and RGB palette targets as `constexpr`. Colors are converted to RGB565 at runtime in `initPalette()` with an R/B swap for BGR panels.
   - `runway_overlay` -- Renders runways from the embedded dataset for whichever tiers the Airports setting selects. Clips lines and labels to the outer ring.
   - `target_select` -- Target picker for social tagging. Entered by double tap; tap cycles outward, 1s hold claims or releases. Tracks the selection by ICAO, not index.
   - `status_screens` -- WiFi setup/connecting/error screens.

4. **`hardware/`** -- Display driver and font loading.
   - `display` -- LovyanGFX init, global `tft` instance.
   - `lgfx_config.hpp` -- SPI bus and GC9A01 panel configuration (pins from `config.h`).
   - `display_font` -- VLW smooth font loader (embedded via `board_build.embed_files`).

### Data

- `include/data/airports.h` / `src/data/airports_data.cpp` -- Embedded airport and runway dataset from OurAirports, split into `large` (1160 airports), `medium` (3801) and `small` (24238) namespaces. The Airports menu setting is cumulative: each level adds its tier on top of the lower ones. Coordinates are `int32_t` E7. Regenerate with `python3 scripts/build_airports.py`; `scripts/test_build_airports.py` covers the generator. `Airport::ident` is a fixed 4 chars and is deliberately **not** null-terminated, so it must be copied into a terminated buffer before being used as a C string.
- `include/data/military_ranges.h` -- Military ICAO hex ranges from tar1090-db, sorted by start and searched with an early break. Header-only, so `isMilitary()` is available to host tests.

### Key design details

- **Two threads, not one**: UI, button and WiFi all run in the Arduino `loop()`, but the fetch runs on a dedicated FreeRTOS task created in `adsb_client::fetchInit()` ("adsb", 8 KB stack, priority 1), woken by a task notification from `fetchStartAsync()`. This is what keeps the display rendering while a poll is in flight. The rules that follow from it:
  - `s_aircraft[]` is written by the fetch task and read by the render loop with no mutex. A frame may show a half-updated array, which is acceptable for a radar; anything that cannot tolerate that needs its own synchronisation.
  - Never touch the radio or the HTTP client while `fetchAsyncBusy()` is true. `resetConnection()` refuses outright, and `main.cpp` gates reconnect attempts on it, because a WiFi teardown and the fetch task must not be inside the network stack together.
  - `fetchInit()` returning false leaves the task handle null and `fetchStartAsync()` becomes a no-op, so the radar renders but stays empty rather than deadlocking on a latched busy flag.
  - The BOOT button uses an ISR (`IRAM_ATTR`) that sets flags consumed by polling in `loop()`.
- **Poll callback**: `adsb_client` accepts a `PollFn` (set to `wifiLoop`) so the WiFiManager web portal stays responsive during synchronous HTTP requests. The fetch task nulls it for the duration of its own fetch: calling `wifiLoop()` off the Arduino task would drive the portal's server from the wrong thread.
- **NVS namespaces**: `"wifi"` for the force-portal flag, `"radar"` for lat/lon, `"planeradar"` for every menu setting, `"social"` for the device secret, the wanted handle and the server-granted handle. Keys are legacy in places: `showRwys` holds the 0-3 airport *mode*, not a bool, so reading it as one gives "large only" for every tier.
- **WiFi self-heals rather than sitting in a portal**: a drop gets `kWifiDownGraceMs` of grace, then background reconnects every `kWifiReconnectIntervalMs`, then a reboot after `kWifiRebootAfterDownMs` (3 min) because a cold boot reconnects reliably where an in-loop retry may not. `kWifiCountryCode` is set explicitly ("NZ", channels 1-13): the ESP32's default world profile only actively scans 1-11, so a router on channel 12 or 13 is never found and it presents as an intermittent failure to connect to a hidden SSID.
- **Social tags**: the feed server in `server/` (self-hosted Docker container) serves aircraft and tags in one response, so the device holds one connection to one host and the tag POST rides the feed's keep-alive socket. `adsb_client` picks TLS or plain HTTP by URL scheme, so a LAN address needs no other change. The device always falls back to fetching adsb.fi directly after repeated failures, which is what keeps a radar working when the server is down. See `docs/social-tags-design.md`.
- **Smoothed motion**: the feed lands every 3-10s but the screen redraws at 20 fps, so aircraft are drawn at dead-reckoned positions, not reported ones. It is the Poll Rate menu's "Smooth" entry (index 0, the default), which is a 3s poll plus dead reckoning; the plain 3s/5s/10s entries draw reported positions and step once per poll. Because "Smooth" took index 0 the old indices shifted, so the setting reads a new NVS key (`pollMode`) and derives from the legacy `pollRate` when it is absent: a stored 1 means 5s under the old numbering and 3s under the new, and nothing in the value says which. Three things this depends on, all easy to break: `ui::motion::onSnapshot()` is called from `main.cpp` at the one point the array is known to be complete (right after `fetchAsyncConsumeResult()`), never from the fetch task; a repeated position is *not* treated as a new fix, because pooled server cells return identical coordinates for several polls and restamping them would drag every aircraft back; and the PR1 header's `pos_age_ms` is what puts the baseline at the right instant, so a server that omits it costs smoothness on the correction step. Extrapolation stops at `kMaxExtrapolationSec` (12s), which is why a dead feed freezes the picture instead of flying it away.
- **Label placement is sticky**: with the symbols moving every frame, `resolveLabels()` keeps last frame's side for each ICAO unless another beats it by a quarter of a label's area. Without that, two near-tied candidate positions flip back and forth and the label visibly flickers.
- **Feed URL is a build flag**: `RADAR_FEED_PROXY_URL`, empty by default, set via `RADAR_FEED_URL` in the `local` env. No machine-specific address belongs in the repo.
- **GC9A01 BGR quirk**: the panel uses BGR subpixel order. `initPalette()` swaps R and B channels in `color565()` calls when `kDisplayRgbOrder` is true, so logical red appears red on screen.
- **Partition layout**: custom 4 MB partition table (`partitions/plane_radar.csv`) with a single 3 MB app slot (no OTA), 896 KB SPIFFS, and a coredump partition.
- **WiFi TX power**: set to 11 dBm in both AP and STA modes. The ESP32-C3 Super Mini has thermal issues at higher power due to poor antenna matching on cheap boards; 11 dBm is safe in sealed PETG enclosures (PETG Tg ~80°C, chip max 85°C). Do not exceed ~15 dBm without venting.
- **Build version**: `scripts/build_version.py` injects the git hash as `BUILD_GIT_HASH` at compile time. Shown on the setup screen and serial boot line.
- **Data freshness**: the center dot changes color (green/amber/red) based on consecutive ADS-B fetch failures. Controlled via `radarDisplaySetFetchFailures()`.
- **Portal timeout**: `kWifiPortalTimeoutSec` is 300s (5 min). On timeout, the device reboots to retry saved credentials, creating a self-healing loop.
- **Runway caching**: `runway_overlay.cpp` caches runway screen coordinates in static arrays, rebuilt only when the range preset, heading, airport tier or radar location changes (location is in the key because the LAN config portal can rewrite it without a reboot). Avoids per-frame distance and projection maths across up to ~28,000 runways once the small tier is on. Per-tier tracking uses packed `uint32_t` bitfields rather than `bool[]` to keep that at ~6 KB instead of ~50 KB, which matters against the mbedTLS buffers.
- **Persistent TLS**: `adsb_client.cpp` reuses a static `WiFiClientSecure` + `HTTPClient` with `setReuse(true)` to avoid a full TLS handshake every 3s poll.
- **VLW font pitfall**: any code that calls `tft.setFont()` with a bitmap font replaces the active VLW smooth font. Call `displayFontEnsureLoaded(tft)` before drawing text that expects VLW.

## Configuration

All hardware pins, timing constants, WiFi behavior, and default coordinates are in `include/config.h`. Radar layout and color palette are in `include/ui/radar_theme.h`. Range presets are in `include/ui/radar_range.h`.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) ^1.2.7 -- display driver
- [WiFiManager](https://github.com/tzapu/WiFiManager) ^2.0.17 -- captive portal
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) ^7.4.2 -- ADS-B JSON parsing
