# Tethered mode investigation: ADS-B over USB instead of WiFi

Status: design and feasibility study. Nothing here is implemented. This document describes what a
"tethered mode" would take, where it plugs into the current firmware, and the constraints that shape
the design.

## Goal

Instead of the device fetching ADS-B from `opendata.adsb.fi` over WiFi, a host computer on the other
end of the USB-C cable streams aircraft data to the device over the serial link. The device renders it
on the round GC9A01 display exactly as it does now. In tethered mode the WiFi radio is off.

## 0. A note on this branch (read first)

The task brief refers to `src/services/adsb_parse.cpp` and `include/services/adsb_parse.h`, a JSON to
`Aircraft` parser that is decoupled from HTTP and unit-tested in a `native` PlatformIO env. Those files
do not exist on this worktree's base (it branched from `origin/main`). On `main` the parse logic is
inline and `static` inside `src/services/adsb_client.cpp::fetchUpdate()` (lines 300-345), coupled to
`HTTPClient` / `WiFiClientSecure` and to an ArduinoJson stream filter.

The decoupled parser does exist on sibling branches: `test/ci-native-and-unit-tests` (commit `ffd4faa`)
ships `include/services/adsb_parse.h`, `src/services/adsb_parse.cpp`, and
`test/test_native/test_adsb_parse.cpp`. That header exposes exactly the field pickers this design wants
to reuse:

```
float pickNoseHeading(const JsonObject& plane);
float pickTrackHeading(const JsonObject& plane);
float pickGroundSpeed(const JsonObject& plane);
bool  isOnGround(const JsonObject& plane);
void  fillTagFields(Aircraft* ac, const JsonObject& plane);
```

Implication for the plan: tethered mode should be built on top of (or after merging) that decoupling.
If it lands on `main` first, Phase 0 below is "extract the parse loop from `fetchUpdate` into a pure
function," which is work someone has already done on another branch. This is called out again in the
phased plan.

## 1. Feasibility verdict

Feasible, and comfortably so. The payload is tiny relative to what the ESP32-C3's USB Serial/JTAG
controller can move, the render path needs no change, and the data model (`Aircraft[64]`) is already the
single source of truth for the UI. The awkward part is not throughput or CPU. It is that on this chip
the one USB channel is shared by flashing, the serial monitor, boot logs, and now the data feed, so the
design has to solve log-versus-data demultiplexing and be robust to the port re-enumerating on reset.

Main constraints:

- The ESP32-C3 has a fixed-function USB Serial/JTAG controller, not USB-OTG. It exposes exactly one
  CDC-ACM serial port plus JTAG and cannot be reconfigured into a second, independent USB data channel.
  See section 2. (Source: Espressif ESP-IDF USB Serial/JTAG console guide.)
- With `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` (set in `platformio.ini` lines 20-21), the
  Arduino `Serial` object is that USB CDC port. It is the same port used to flash and to run
  `pio device monitor`.
- The firmware is single-threaded in `loop()`. Serial reads must be non-blocking so the 50 ms frame
  cadence (`kFrameIntervalMs` in `src/main.cpp` line 26) is not stalled.

## 2. The USB / serial channel

### What `Serial` is on this board

The Super Mini uses the C3's built-in USB Serial/JTAG controller. It is a CDC-ACM virtual serial port
implemented entirely in hardware, directly on the USB-C connector, with no external USB-to-UART bridge.
Espressif is explicit that it "is a fixed-function USB device that is implemented entirely in hardware,
meaning that it cannot be reconfigured to perform any function other than a serial port and JTAG
debugging functionality." That rules out the S2/S3 trick of spinning up a second TinyUSB CDC interface
for a separate data pipe. On the C3 there is one CDC port, full stop.

With the two USB build flags this project already sets, the Arduino core maps `Serial` to that USB CDC
port (`ARDUINO_HW_CDC_ON_BOOT`), and maps `Serial0` to the chip's hardware UART on GPIO pins. So the
data feed, the boot banner, and every `Serial.printf` in the codebase all land on the same USB pipe that
esptool and the serial monitor use.

Sources:
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/usb-serial-jtag-console.html
- https://documentation.espressif.com/esp32-c3_datasheet_en.html
- https://community.platformio.org/t/enabling-usb-cdc-on-boot-on-esp32-c3-devkit/33346

### Throughput headroom

Baud rate is irrelevant here. The USB Serial/JTAG controller ignores the configured baud (`monitor_speed
= 115200` is cosmetic for this port) and moves data at USB Full-Speed, which is a 12 Mbit/s line rate,
roughly 1.5 MB/s raw before protocol overhead. Measured application throughput is lower, but flashing
benchmarks over this same interface run around 435 kbit/s effective (compressed image write), so a
sustained few tens of KB/s is well within reach.

Now the payload. The UI needs at most `kMaxAircraft = 64` records. Each `Aircraft`
(`include/services/adsb_client.h` lines 7-17) is five floats plus `callsign[9]`, `type[5]`, `alt[12]`,
and a bool, about 47 bytes packed. A full 64-aircraft binary frame is roughly 3 KB. As JSON in the
adsb.fi shape (only the filtered fields), a busy sky is more like 4-8 KB. At the default 3 s poll
(`kAdsbFetchIntervalMs`, and the menu-selectable 3/5/10 s in `radar_range.cpp` line 25) that is on the
order of 1-3 KB/s. Two to three orders of magnitude under the practical ceiling. Throughput is a
non-issue.

Sources:
- https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html
- https://documentation.espressif.com/esp32-c3_datasheet_en.html

### RX buffering and the blocking quirk

One behavioral difference from a real UART bridge matters. Espressif notes that a USB-to-serial bridge
chip will happily throw bytes at a chip that is not reading, whereas the USB Serial/JTAG controller
"will block until the application reads the bytes." In practice the host's write blocks (back-pressure)
until `loop()` drains the RX side. Because `loop()` reads every iteration (every ~5-55 ms), back-pressure
is bounded, but the host should send once per poll interval, not in a tight spin loop. On the TX side the
controller uses a small internal buffer (community reports ~256 bytes) that can overwrite old data if the
app floods it, and it flushes automatically after a newline on the default driver. That flush-on-newline
behavior is convenient for a newline-delimited protocol.

### The log-versus-data sharing problem

This is the real design constraint. Logs (`Serial.printf` / `Serial.println`) and the data stream cannot
have separate USB endpoints on the C3. Three ways to make them coexist, best to worst for this project:

1. Frame the data and make the device ignore anything that is not a valid frame. Boot banner and any
   stray logs are simply skipped by the reader. This is robust and needs no discipline from other code.
2. Gate logging in tethered mode. Wrap the noisy `Serial.printf` calls so they are silent (or downgraded)
   once tethered mode is active, leaving the channel clean for data. Simple, but easy to regress when new
   log lines are added.
3. Route logs off USB. Because `Serial0` maps to the hardware UART, diagnostic logging in tethered mode
   could go to `Serial0` (UART pins, seen only with a separate adapter) while USB `Serial` carries pure
   data. Clean separation over the one USB cable, at the cost of needing a UART header to see logs.

Recommendation: do (1) as the baseline (the reader must tolerate garbage regardless), and add (2) for the
handful of hot-path logs so the channel stays quiet even if a frame check is ever loosened. Option (3) is
a nice-to-have for developers.

## 3. Wire protocol proposal

Two candidates were considered.

### Option A (recommended): sentinel-prefixed NDJSON

One JSON document per line, in the exact adsb.fi `{"ac":[...]}` shape, prefixed with a fixed sentinel so
the device can tell data lines from log noise and resynchronise on any newline.

```
ADSB1 {"ac":[{"lat":52.31,"lon":4.76,"hex":"484123","flight":"KLM123",
"t":"B738","alt_baro":30000,"gs":420,"track":85,"true_heading":83}]}\n
```

Rules:
- A data line starts with the literal `ADSB1 ` (protocol id + version + space) and ends with `\n`.
- Everything between the sentinel and the newline is one complete JSON object with a top-level `ac`
  array, identical to what adsb.fi returns and what `fetchUpdate` already deserializes.
- The device ignores any line that does not start with the sentinel (boot banner, logs, blank lines).
- Optional integrity: append ` *HHHH` before the newline, a CRC-16/CCITT over the JSON bytes, hex-encoded.
  The device drops the line if the CRC does not match. Start without it; add it if field testing shows
  corruption.

Why this wins: it reuses the existing parser verbatim. The JSON field names, the ArduinoJson filter
(`initJsonFilter`, lines 40-60), the field pickers, the `is_military` hex lookup (lines 333-337), and the
ground-aircraft filter (`isOnGround`) all apply unchanged because the host sends the same JSON the device
already knows how to read. The host stays trivial. The stream is human-readable, so debugging is just
watching the port. The cost is a few extra bytes per line versus binary, which the throughput budget does
not care about.

Reuse of `adsb_parse.cpp`: yes, as-is, on the branches where it exists. The injection entry point (section
4) hands a `JsonObject` per aircraft to the same `pickNoseHeading` / `pickTrackHeading` / `pickGroundSpeed`
/ `fillTagFields` helpers. On `main`, the equivalent inline loop in `fetchUpdate` (lines 315-342) would be
the code to factor out and share (Phase 0).

### Option B: framed binary records

A compact binary frame with sync bytes, length, and CRC. Robust resync and smallest payload, but it does
not reuse the JSON parser: it needs a new binary decoder on the device and a new encoder on the host.

```
Frame:
  0xA5 0x5A            magic (2 bytes)
  0x01                 version (1 byte)
  uint8   count        number of records, 0..64
  uint16  payload_len  little-endian, count * sizeof(record)
  record[count]        packed records (see below)
  uint16  crc16        CRC-16/CCITT over version .. last record byte

Record (little-endian), one per aircraft, mapping directly to struct Aircraft:
  float32 lat
  float32 lon
  float32 nose_deg
  float32 track_deg
  float32 gs_knots
  char    callsign[9]   NUL-padded
  char    type[5]       NUL-padded
  char    alt[12]       NUL-padded, e.g. "30000 ft" or "GND"
  uint8   is_military   0 or 1
  (47 bytes; pad to 48 for 4-byte alignment if copied straight into the array)
```

Note on `is_military`: today it is derived on-device from the ICAO `hex` via
`data::military::isMilitary` (`adsb_client.cpp` lines 333-337). Binary records skip `hex`, so either the
host precomputes `is_military` (duplicating the range table off-device) or a `hex` field is added back to
the record. NDJSON avoids this entirely by keeping `hex` in the JSON so the existing on-device lookup
still runs.

Choose Option B only if logs genuinely cannot be kept off the channel and a hard integrity guarantee is
required, or if a much higher record rate is ever needed. For this project, Option A is the right call.

## 4. Firmware integration points

### 4a. A data-injection API on `adsb_client`

Today the array is private and read-only from outside: `s_aircraft[kMaxAircraft]` and `s_aircraft_count`
are file-static in `adsb_client.cpp` (lines 27-28), exposed only through `aircraftCount()` /
`aircraftList()` (header lines 21-22). Tethered mode needs a way to write into that array without going
through HTTP.

Minimal new API (described, not written), added to `include/services/adsb_client.h` and implemented in
`adsb_client.cpp`:

- Preferred, reuses the parser: `bool ingestJson(const char* json, size_t len);` It runs the same
  `initJsonFilter` + `deserializeJson(..., Filter(...))` + the `for (JsonObject plane : ac)` loop that
  `fetchUpdate` runs (lines 300-343), but sourced from the passed buffer instead of the HTTP stream. It
  writes `s_aircraft` / `s_aircraft_count` and returns success. This keeps all field logic in one place.
- Lower-level alternative if you prefer the caller to own parsing: `Aircraft* aircraftMutableBuffer();`
  returning `s_aircraft`, plus `void setAircraftCount(size_t n);` clamped to `kMaxAircraft`. The serial
  module fills the buffer and sets the count.

Either way the render path is untouched: `radarDisplayRefreshAircraft()` reads `aircraftList()` /
`aircraftCount()` as it does now. In tethered mode the async fetch task (`fetchInit`,
`fetchStartAsync`, the FreeRTOS task at lines 357-384) is simply never started; parsing a few KB of JSON
inline in `loop()` costs single-digit milliseconds and fits the frame budget, so no background task is
needed.

### 4b. `main.cpp` loop changes

`setup()` and `loop()` (`src/main.cpp`) currently assume WiFi. The change is to branch on the selected
mode.

- `setup()`: if tethered, skip `wifiSetupConnect()` (line 114), skip `services::adsb::fetchInit()` (line
  112) or leave the task idle, turn the radio off (4c), and mark the radar visible so the render path runs
  without waiting on `WL_CONNECTED`.
- `loop()`: replace the WiFi block (lines 124-167) with a serial pump when tethered. Concretely: a small
  line-assembler that on each iteration does non-blocking `while (Serial.available()) { ... }`, appends
  bytes to a bounded buffer, and on `\n` validates the sentinel (and optional CRC) and calls
  `services::adsb::ingestJson(...)`. On a successful ingest, reset the staleness timer (4d). The existing
  frame-cadence block (lines 169-173) stays as is and keeps calling `radarDisplayRefreshAircraft()`.
- `handleBootButton()` and the menu keep working unchanged, so range, heading, labels, etc. still adjust
  live.

Keep the reads non-blocking. Never use `Serial.readStringUntil` or any call with a timeout that can stall
the loop, or the 50 ms frame cadence and BOOT-button responsiveness suffer.

### 4c. Disabling WiFi and the thermal upside

In tethered mode call `WiFi.mode(WIFI_OFF)` in `setup()` (and do not start WiFiManager). With the radio
off, the board's biggest heat source is gone. CLAUDE.md documents that the C3 Super Mini has thermal
issues at higher WiFi TX power due to poor antenna matching on cheap boards, which is why TX power is
pinned to 11 dBm and the enclosure guidance warns about PETG's ~80 C glass transition against the chip's
85 C max. Tethered mode sidesteps that class of problem: no transmit duty cycle, lower and steadier power
draw, cooler running in a sealed enclosure. It also removes WiFi credential setup and the reconnect /
self-heal reboot loop for anyone who only ever runs it tethered.

### 4d. Data-freshness center dot

The freshness indicator is driven by `ui::radarDisplaySetFetchFailures(uint8_t)`
(`include/ui/radar_display.h` line 14). In WiFi mode `main.cpp` maintains
`g_consecutive_fetch_failures` in `handleAsyncFetchResult()` (lines 81-94): 0 on success, incremented on
each failed fetch, pushed into the display so the dot goes green, amber, red.

Tethered mode maps cleanly onto the same counter, just with a different trigger. Track
`g_last_frame_rx_ms`. Each loop, compute the age since the last valid ingest. Translate that age into the
same 0/amber/red scale, for example: fresh within one poll interval -> 0 (green), stale past two or three
intervals -> a small positive count (amber), well past that -> a high count (red). Push it via
`radarDisplaySetFetchFailures(...)` exactly as the WiFi path does. No display change needed; only the
source of the number changes from "consecutive HTTP failures" to "time since last serial frame."

## 5. Mode selection

Three approaches, weighed for this device (headless, one button, sealed enclosure, occasional flashing):

- (a) Persisted NVS setting toggled from the on-device menu. Follows the established pattern: a value in
  the `planeradar` namespace like the existing `sweep` / `mil` keys (`radar_range.cpp` lines 21, 26 and
  the getter/setter pattern at lines 201-213), surfaced as a menu item in `menu.cpp`'s `kMenuItems` array
  (lines 59-70) with labels. Deterministic and user-visible. Costs a menu entry and a reboot to apply
  cleanly (WiFi vs no-WiFi is decided at boot).
- (b) Auto-detect. Boot normally, and if a valid sentinel frame arrives on serial (within N ms of boot,
  or at any time), switch to tethered and turn WiFi off; otherwise run WiFi as today. Zero configuration,
  and the sentinel prevents the boot banner or a serial monitor from false-triggering it. The wrinkle is a
  runtime WiFi teardown when a frame arrives mid-session, which is more state to get right than a
  boot-time decision.
- (c) Compile-time flag / separate PlatformIO env. A `-DTETHERED_MODE` build or an `[env:tethered]`
  target. Cleanest code paths and smallest binary, but end users would need to flash a different image to
  switch, which is poor UX for a general release. Useful as a dedicated demo/kiosk firmware.

Recommendation: a three-state NVS setting selected from the menu, `Mode = Auto / WiFi / Tethered`,
defaulting to `Auto`, where `Auto` implements (b) on top of (a).

- `Auto` (default): start WiFi as today, but the serial pump is always listening; the first valid frame
  flips the device to tethered and powers the radio down. If no frame ever arrives, it behaves exactly
  like the current firmware. This gives plug-and-play tethering with no user action.
- `Tethered`: never bring WiFi up; radio off from boot. Best thermals, fastest boot to radar.
- `WiFi`: current behavior, serial pump disabled, so `pio device monitor` and logs are unaffected.

This combines the determinism of (a) for anyone who wants a fixed mode with the zero-config convenience of
(b) for everyone else, and it reuses the exact NVS + menu machinery already in the codebase. Keep (c)
available as an optional dedicated build, not the primary mechanism.

## 6. Host-side tooling (sketch)

A small Python script reads a local source and streams sentinel-prefixed NDJSON at the poll cadence. Two
common sources: a local `readsb` / `dump1090-fa` `aircraft.json`, or the adsb.fi API directly. Note the
top-level key differs: readsb/dump1090 use `{"aircraft":[...]}`, adsb.fi uses `{"ac":[...]}`. The device
parser expects `ac`, so the host re-wraps under `ac`. The per-aircraft field names (`hex`, `flight`,
`alt_baro`, `gs`, `track`, `true_heading`, `mag_heading`, `lat`, `lon`, `t`) share the same readsb
lineage, so no per-field remapping is needed.

Sources for the field shapes:
- https://github.com/wiedehopf/readsb/blob/dev/README-json.md
- https://github.com/flightaware/dump1090/blob/master/README-json.md

```python
# tether_feed.py  (sketch, not production)
import json, time, sys
import serial          # pyserial
import urllib.request  # or read a local aircraft.json file

PORT = "/dev/ttyACM0"   # the C3's USB CDC port
POLL_S = 3
SENTINEL = b"ADSB1 "

def fetch_aircraft():
    # Source 1: local readsb / dump1090-fa
    #   with open("/run/dump1090-fa/aircraft.json") as f:
    #       return json.load(f).get("aircraft", [])
    # Source 2: adsb.fi API (already {"ac":[...]})
    url = "https://opendata.adsb.fi/api/v3/lat/52.31/lon/4.76/dist/25"
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.load(r).get("ac", [])

def to_frame(aircraft):
    # Cap at the device limit and keep only fields the device parses.
    keep = ("lat", "lon", "hex", "flight", "t", "alt_baro", "alt_geom",
            "gs", "tas", "ias", "track", "true_heading", "mag_heading", "dir")
    slim = [{k: a[k] for k in keep if k in a} for a in aircraft[:64]]
    body = json.dumps({"ac": slim}, separators=(",", ":")).encode()
    return SENTINEL + body + b"\n"      # add optional CRC before \n if used

def main():
    with serial.Serial(PORT, 115200, timeout=1) as ser:  # baud ignored by C3 CDC
        while True:
            try:
                ser.write(to_frame(fetch_aircraft()))
                ser.flush()
            except Exception as e:
                print("feed error:", e, file=sys.stderr)  # do not spam the port
            time.sleep(POLL_S)

if __name__ == "__main__":
    main()
```

Practical notes for the host: send once per interval (do not spin, because the C3 back-pressures the host
write until the device reads); reopen the port if it disappears on device reset (section 7); and do not run
`pio device monitor` at the same time, since both would fight over the one port.

## 7. Risks and open questions

- Serial monitor collision. The data feed, `pio device monitor`, and esptool all use the one USB CDC
  port. Only one program can own it. Document that tethering and monitoring are mutually exclusive, and
  that logs in tethered mode should be gated or sent to `Serial0`.
- USB re-enumeration on reset. Any reset (including esptool's auto-reset for flashing, and any
  `esp_restart()` such as the WiFi self-heal reboot) drops and re-creates the CDC port, so `/dev/ttyACM*`
  briefly disappears. The host feeder must detect the drop and reopen. Deep sleep and light sleep also
  make the device vanish from the host, and light sleep can fail to re-enumerate without a cable
  replug; tethered mode should avoid sleep. (Espressif USB Serial/JTAG console guide.)
- Blocking reads in the single-threaded loop. Reads must be non-blocking (`Serial.available()` guarded,
  no timeout waits) or the 50 ms frame cadence and BOOT-button handling stall. This is the single easiest
  way to get the integration wrong.
- Buffer overflow / oversized lines. The line assembler needs a hard cap (for example 12 KB, above the
  worst-case 64-aircraft JSON) and must drop and resync on overflow rather than growing unbounded on RAM
  that is already tight (see the mbedTLS heap notes in `adsb_client.cpp` lines 258-262 for how little
  slack there is).
- Framing and resync. With NDJSON, resync is "discard until newline, then require the sentinel." A
  half-received line after a reset is dropped cleanly. The optional CRC guards against a corrupt-but-valid
  looking JSON line.
- Back-pressure. The C3 blocks the host write until the app reads. The loop reads every iteration so this
  is bounded, but a host that writes faster than the poll interval will see its writes stall. Send at the
  poll cadence.
- Flipping between WiFi and tethered. In `Auto`, a mid-session switch to tethered means tearing down WiFi
  at runtime, which must not run while a fetch is in flight (the same hazard the existing reconnect logic
  guards with `fetchAsyncBusy()`, `main.cpp` line 148). Simplest safe answer: on first valid frame, set
  the NVS mode and `esp_restart()` into a clean tethered boot, rather than tearing WiFi down live.
- `is_military` provenance in binary mode. Resolved by NDJSON (keep `hex`, on-device lookup runs). Only an
  issue if Option B is chosen.
- TX log buffer overwrite. The ~256-byte CDC TX buffer can drop log bytes under flooding; another reason
  to keep logs quiet or off-USB in tethered mode.

## 8. Recommended phased implementation plan

Small, reviewable steps. Do not implement any of this yet.

- Phase 0 (prerequisite): make the JSON to `Aircraft` parse a pure function callable without HTTP. Either
  rebase tethered work onto `test/ci-native-and-unit-tests` (which already has `adsb_parse.cpp` and native
  tests), or extract the loop in `fetchUpdate` (lines 300-343) into a shared function. Keep the native
  unit tests green.
- Phase 1: add the injection API to `adsb_client` (`ingestJson(const char*, size_t)` preferred), writing
  `s_aircraft` / `s_aircraft_count`. Unit-test it on host with in-memory JSON.
- Phase 2: add a `serial_feed` module: a non-blocking, bounded line assembler that validates the sentinel
  (and optional CRC) and calls `ingestJson`. Unit-test the assembler in the native env (feed it bytes in
  odd chunk sizes, with noise and partial lines, assert correct extraction).
- Phase 3: `main.cpp` plumbing behind a mode flag: WiFi off in tethered, serial pump in `loop()`,
  staleness-to-`setFetchFailures` mapping, render path unchanged. Prove it with a fixed compile-time flag
  first to isolate the loop changes from the settings work.
- Phase 4: mode selection as a three-state NVS setting (`Auto`/`WiFi`/`Tethered`) using the
  `radar_range.cpp` NVS pattern and a `menu.cpp` entry; implement `Auto` (listen always, switch on first
  valid frame via a clean restart).
- Phase 5: the Python host feeder (readsb `aircraft.json` and adsb.fi sources), with port-reopen on device
  reset.
- Phase 6: field test on hardware (throughput sanity, reset/reopen behavior, thermals with the radio off),
  then document usage in the README.

## Sources

- ESP-IDF, USB Serial/JTAG Controller Console (ESP32-C3):
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/usb-serial-jtag-console.html
- ESP-FAQ, USB peripherals:
  https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html
- ESP32-C3 datasheet (USB Serial/JTAG, 12 Mbit/s Full-Speed):
  https://documentation.espressif.com/esp32-c3_datasheet_en.html
- PlatformIO community, enabling USB CDC on boot for the C3:
  https://community.platformio.org/t/enabling-usb-cdc-on-boot-on-esp32-c3-devkit/33346
- readsb aircraft.json field reference:
  https://github.com/wiedehopf/readsb/blob/dev/README-json.md
- dump1090-fa aircraft.json field reference:
  https://github.com/flightaware/dump1090/blob/master/README-json.md
