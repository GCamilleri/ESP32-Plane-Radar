#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 300;  // 5 min then reboot to retry
constexpr unsigned long kWifiConnectingFrameMs = 50;

/**
 * 2.4 GHz regulatory region for channel scanning. NZ/EU/AU permit channels
 * 1-13. The ESP32 default "01" world profile only actively scans 1-11, so a
 * router that auto-selects channel 12 or 13 is never found (reason 201,
 * NO_AP_FOUND), which shows up as intermittent connects to hidden SSIDs.
 * Setting a manual policy forces active scanning across the full range.
 * US users should set the count to 11.
 */
constexpr char kWifiCountryCode[] = "NZ";
constexpr uint8_t kWifiCountryStartChannel = 1;
constexpr uint8_t kWifiCountryChannelCount = 13;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;
/**
 * If the link stays down this long, reboot to recover. A cold boot reliably
 * reconnects, so this is a self-heal backstop for states the in-loop reconnect
 * cannot clear on its own (e.g. a wedged radio or a stale DHCP lease).
 */
constexpr unsigned long kWifiRebootAfterDownMs = 180000;  // 3 min

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/** Taps longer than this are treated as holds, not taps. */
constexpr unsigned long kBootTapMaxMs = 800UL;
/** Wait after last tap before reporting gesture (multi-tap detection window). */
constexpr unsigned long kBootGestureDebounceMs = 400UL;
/** Hold duration for menu select/back action. */
constexpr unsigned long kBootShortHoldMs = 1000UL;

// --- Menu ---
/** Exit menu after this many ms of no interaction. */
constexpr unsigned long kMenuTimeoutMs = 4000UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Default poll interval (ms). Runtime value set via menu; see radar_range.h. */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- UI colors (RGB565) for status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
