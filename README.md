# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/GCamilleri/ESP32-Plane-Radar/releases)

Live ADS-B radar on an **ESP32-C3 Super Mini** with a **1.28" round GC9A01** display. Shows aircraft positions, headings, and altitude around your location using data from [adsb.fi](https://opendata.adsb.fi/).

Fork of [WatskeBart/ESP32-Plane-Radar](https://github.com/WatskeBart/ESP32-Plane-Radar).

## Features

- Real-time aircraft tracking with heading indicators, speed vectors, and callsign/altitude tags
- Military aircraft highlighting (identified by ICAO hex ranges)
- Animated radar sweep line
- Airport runway overlay with three tiers (~29,000 airports worldwide)
- Automatic label deconfliction to prevent overlapping text
- On-device settings menu (range, heading, labels, airports, poll rate, sweep, military)
- Configurable ADS-B poll rate (3s / 5s / 10s)
- Non-blocking network requests, so the display keeps rendering while data loads

## Hardware

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO 0 |
| CS | GPIO 1 |
| DC | GPIO 10 |
| SDA | GPIO 3 |
| SCL | GPIO 4 |

## Installation

### Option 1: Web installer

Open [gcamilleri.github.io/ESP32-Plane-Radar](https://gcamilleri.github.io/ESP32-Plane-Radar/) in **Chrome or Edge** on desktop, plug in the board, and click **Install**. The flashing runs in the browser over Web Serial.

### Option 2: Manual flash

Download `plane-radar-v*.bin` from [Releases](https://github.com/GCamilleri/ESP32-Plane-Radar/releases) and flash at address `0x0`:

```bash
pip install esptool
esptool.py --chip esp32c3 --baud 460800 write_flash 0x0 plane-radar-v1.5.0-gc.bin
```

Or use the generic [esptool-js web flasher](https://espressif.github.io/esptool-js/): connect, set address `0x0`, select the `.bin`, and program.

### Option 3: Build from source

```bash
git clone https://github.com/GCamilleri/ESP32-Plane-Radar.git
cd ESP32-Plane-Radar
pio run -t upload        # build + flash
pio device monitor       # serial monitor (115200 baud)
```

## Updating

Use the [web installer](https://gcamilleri.github.io/ESP32-Plane-Radar/) and choose **Keep settings** when prompted. Your WiFi and preferences are preserved.

Prefer the command line? Flash the **app-only** image at `0x10000` (leaves your settings, stored below that address, untouched):

```bash
esptool.py --chip esp32c3 --baud 460800 write_flash 0x10000 plane-radar-app-v1.5.0-gc.bin
```

> Most releases update this way. If a release note says a full reflash is required (e.g. the partition layout changed), do a fresh install instead -- that resets settings.

## Setup

On first boot, the device creates a WiFi access point called **PlaneRadar-Setup**:

1. Connect to the AP from your phone or computer
2. A captive portal opens (or browse to `http://192.168.4.1`)
3. Select your home WiFi network and enter the password
4. Set your location (latitude/longitude) and preferences
5. Save -- the device reboots, connects to WiFi, and starts the radar

Once setup finishes the radar stops transmitting a network of its own, so it adds no 2.4 GHz traffic beyond its own connection.

To reconfigure later, switch **LAN Cfg** to On in the settings menu. That screen then shows the address to visit (`http://plane-radar.local`, and the radar's IP in case your network blocks mDNS) from any device on the same network. It is a web page on the radar's existing connection, not an access point, so no new SSID appears. Switch it back off when you are done.

## Controls

All interaction is through the **BOOT button** (GPIO 9):

| Action | Effect |
|--------|--------|
| **Tap** | Cycle range (5 / 10 / 15 / 25 km) |
| **Hold ~1s** | Open settings menu |
| **Hold 3s** (setup or connecting screen) | Reset WiFi credentials and reboot into setup |

In the settings menu, **tap** moves to the next item and **hold** selects or goes back. The menu pages automatically and closes after 4 seconds of inactivity.

Once the radar is up, a hold opens the menu at 1s, so the 3s reset does not apply there: use the menu's **Reset WiFi** entry instead. The 3s hold is for the setup and connecting screens, where there is no menu to open.

## Troubleshooting

- **Board not detected** -- use a data-capable USB-C cable, not charge-only
- **Permission denied (Linux)** -- `sudo usermod -aG dialout $USER` then log out/in
- **WiFi won't connect** -- hold BOOT for 3+ seconds on the connecting screen to reset and re-enter setup, or use **Reset WiFi** in the settings menu
- **No aircraft shown** -- check your coordinates are correct; center dot turns red if fetches are failing

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) -- display driver
- [WiFiManager](https://github.com/tzapu/WiFiManager) -- captive portal
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) -- ADS-B JSON parsing
