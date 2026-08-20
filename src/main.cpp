/**
 * Plane Radar: WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <esp_system.h>

#ifndef BUILD_GIT_HASH
#define BUILD_GIT_HASH "dev"
#endif

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/social_tags.h"
#include "services/wifi_setup.h"
#include "ui/aircraft_motion.h"
#include "ui/menu.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/target_select.h"

namespace {

constexpr unsigned long kFrameIntervalMs = 50;

bool g_radar_visible = false;
bool g_menu_hold_fired = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_frame_ms = 0;
uint8_t g_consecutive_fetch_failures = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  // Hold-to-reset is driven from here, not from wifiLoop(), so the gesture works
  // whenever the radar is running: wifiLoop() only polled it while the LAN config
  // portal was up, and that portal is now off by default.
  // It is disarmed while the menu is open: the menu selects on a 1 s hold, so a
  // slightly long press there must not wipe credentials. The menu carries its
  // own explicit "Reset WiFi" entry for that.
  // Disarmed for the target picker for the same reason as the menu: a hold there
  // claims a tag, so a slightly long press must not wipe credentials.
  bootButtonSetLongPressEnabled(!ui::menu::isOpen() && !ui::target::isOpen());
  bootButtonPollLongPress();

  if (ui::menu::isOpen()) {
    ui::menu::update();
    if (!ui::menu::isOpen()) {
      if (g_radar_visible) {
        ui::radarDisplayDraw();
      }
    }
    return;
  }

  if (ui::target::isOpen()) {
    ui::target::update();
    return;
  }

  // On the radar screen a single tap changes range and a double tap opens the
  // target picker, so both have to go through the gesture API. That costs
  // kBootGestureDebounceMs of latency on a range change: the price of telling one
  // tap from two on a device with a single button.
  const uint8_t taps = bootButtonConsumeGesture();
  if (taps == 1) {
    onRangeTap();
  } else if (taps >= 2 && !ui::target::open()) {
    Serial.println("No aircraft on screen to tag");
  }

  if (bootButtonHeldMs() >= config::kBootShortHoldMs && !g_menu_hold_fired) {
    g_menu_hold_fired = true;
    // Disarm here, not just on the next iteration's gate above: this same
    // iteration can still enter a blocking reconnect, which polls the long
    // press itself and would reach 3 s with the menu already open.
    bootButtonSetLongPressEnabled(false);
    ui::menu::open();
  }
  if (!bootButtonIsHeld()) {
    g_menu_hold_fired = false;
  }
}

void handleAsyncFetchResult() {
  bool ok = false;
  if (!services::adsb::fetchAsyncConsumeResult(&ok)) return;

  if (ok) {
    g_consecutive_fetch_failures = 0;
    ui::radarDisplaySetFetchFailures(0);
    // Here rather than in the fetch task: this is the one moment the array is
    // known to be complete and not being written to.
    ui::motion::onSnapshot(services::adsb::aircraftList(),
                           services::adsb::aircraftCount(), millis(),
                           services::adsb::lastPositionAgeMs());
  } else {
    if (g_consecutive_fetch_failures < 255) {
      ++g_consecutive_fetch_failures;
    }
    ui::radarDisplaySetFetchFailures(g_consecutive_fetch_failures);

    // Heap with the failure, not on request: the failure that matters here is
    // mbedTLS being unable to allocate a session, and largest-free-block is the
    // number that shows it. Logged sparsely, since this repeats every poll.
    if (g_consecutive_fetch_failures == 1 || g_consecutive_fetch_failures == 5 ||
        g_consecutive_fetch_failures % 20 == 0) {
      Serial.printf("adsb: %u consecutive failures, heap %u free, %u largest\n",
                    static_cast<unsigned>(g_consecutive_fetch_failures),
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }

    // Reboot rather than sit there dead. A cold boot gets an unfragmented heap,
    // which is the one thing that reliably clears the mbedTLS allocation failure,
    // and it is the same self-heal the WiFi path already has. Gated on the link
    // being up so a WiFi outage stays with its own timer.
    if (g_consecutive_fetch_failures >= config::kFetchFailuresBeforeReboot &&
        WiFi.status() == WL_CONNECTED) {
      Serial.println("adsb: every fetch failing, rebooting to recover");
      delay(100);
      esp_restart();
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.printf("Plane Radar [%s]\n", BUILD_GIT_HASH);

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  // Before the first fetch: the fetch task registers the device and signs claims,
  // and needs the identity to exist by then.
  services::social::init();
  services::adsb::setPollFn(wifiLoop);
  if (!services::adsb::fetchInit()) {
    Serial.println("Radar will render but stay empty (no fetch task)");
  }

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  // After the first render, so the frame sprite's ~115 KB is already taken: this is
  // the headroom a TLS handshake actually gets, and the number to compare against
  // when a fetch later fails to allocate one.
  Serial.printf("heap after boot: %u free, %u largest\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void loop() {
  handleBootButton();
  wifiLoop();
  handleAsyncFetchResult();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost, will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;

    // Self-heal: a cold boot reliably reconnects, so if the link has been down
    // too long and the in-loop reconnect isn't recovering, reboot.
    if (down_ms >= config::kWifiRebootAfterDownMs) {
      Serial.println("WiFi down too long, rebooting to recover");
      delay(100);
      esp_restart();
    }

    // Only touch the radio when no fetch is in flight: the fetch task and a
    // WiFi teardown must not be inside the network stack at the same time.
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs &&
        !services::adsb::fetchAsyncBusy()) {
      g_last_reconnect_ms = millis();
      services::adsb::resetConnection();  // drop the dead keep-alive socket
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible && !ui::menu::isOpen()) {
      showRadarIfConnected();
    } else if (!services::adsb::fetchAsyncBusy() && !ui::menu::isOpen() &&
               millis() - g_last_adsb_fetch_ms >= ui::radar::pollRateMs()) {
      g_last_adsb_fetch_ms = millis();
      services::adsb::fetchStartAsync(services::location::lat(),
                                      services::location::lon(),
                                      ui::radar::fetchRadiusKm());
    }
  }

  if (g_radar_visible && !ui::menu::isOpen() &&
      millis() - g_last_frame_ms >= kFrameIntervalMs) {
    g_last_frame_ms = millis();
    ui::radarDisplayRefreshAircraft();
  }

  delay(5);
}
