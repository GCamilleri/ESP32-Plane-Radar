#pragma once

#include <cstddef>
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

// --- Social aircraft tags (Cloudflare Worker proxy) ---
/**
 * Worker base URL, no trailing slash. Empty builds a device with no social
 * features: the radar then always fetches adsb.fi directly.
 *
 * Deploy worker/ and put your own workers.dev subdomain here, or override at build
 * time for a locally hosted Worker, which is what `pio run -e local` does:
 *
 *   RADAR_FEED_URL=http://192.168.1.17:8787 pio run -e local -t upload
 *
 * Both http:// and https:// work; adsb_client picks the transport by scheme, so a
 * plain-HTTP LAN address needs no other change.
 */
#ifndef RADAR_FEED_PROXY_URL
#define RADAR_FEED_PROXY_URL ""
#endif
constexpr char kFeedProxyBaseUrl[] = RADAR_FEED_PROXY_URL;

/**
 * Shared key sent as X-Radar-Key on every request to the feed server. Empty omits
 * the header entirely.
 *
 * This is a gate, not authentication: it stops a stranger who finds the hostname
 * from spending your adsb.fi budget or filling the tag table, which is the whole
 * exposure of putting the server on the public internet. Anyone holding a firmware
 * binary can extract it, so it is worth exactly as much as keeping the binary to
 * yourself. Check it at the reverse proxy so unauthorised traffic never reaches the
 * container, and set FEED_KEY on the server too so the check survives a proxy
 * misconfiguration.
 */
#ifndef RADAR_FEED_PROXY_KEY
#define RADAR_FEED_PROXY_KEY ""
#endif
constexpr char kFeedProxyKey[] = RADAR_FEED_PROXY_KEY;
/**
 * Consecutive proxy failures before the device gives up on it and fetches
 * adsb.fi directly instead. The radar must never depend on the Worker being up,
 * so this is the standalone guarantee in code rather than in a comment.
 */
constexpr uint8_t kFeedProxyFailuresBeforeBackoff = 3;
/**
 * Backoff before retrying the proxy, doubling each time it fails again and reset to
 * the base by any successful proxy fetch.
 *
 * Exponential rather than a flat delay because the two cases pull in opposite
 * directions. A server that was restarted for twenty seconds should be picked up
 * again almost immediately, which argues for a short delay; a server that is gone
 * for the weekend should not be probed every few minutes forever, which argues for a
 * long one. Doubling gets both: back within 30s of a blip, and settling at three
 * wasted attempts per quarter hour when it is really down.
 */
constexpr unsigned long kFeedProxyBackoffBaseMs = 30000UL;   // 30 s
constexpr unsigned long kFeedProxyBackoffMaxMs = 900000UL;   // 15 min
/** Exit the target picker after this long with no button activity. */
constexpr unsigned long kTargetSelectTimeoutMs = 6000UL;
/**
 * Give up on a queued claim after this long and report it. Without this a claim
 * made while the proxy is unreachable sits in the queue and the UI shows
 * "tagging..." forever, which reads as a hang rather than a failure.
 */
constexpr unsigned long kSocialRequestTimeoutMs = 20000UL;
/**
 * Bytes of NVS-persisted device secret. Generated once from esp_random(). This is
 * an identity for rate limiting, not a credential: registration is open, so
 * anyone can mint one.
 */
constexpr size_t kSocialSecretBytes = 16;

// --- UI colors (RGB565) for status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
