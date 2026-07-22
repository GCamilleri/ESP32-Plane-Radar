/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#ifndef BUILD_GIT_HASH
#define BUILD_GIT_HASH "dev"
#endif

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/menu.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

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
  if (ui::menu::isOpen()) {
    ui::menu::update();
    if (!ui::menu::isOpen()) {
      if (g_radar_visible) {
        ui::radarDisplayDraw();
      }
    }
    return;
  }

  if (bootButtonConsumeTap()) {
    onRangeTap();
  }

  if (bootButtonHeldMs() >= config::kBootShortHoldMs && !g_menu_hold_fired) {
    g_menu_hold_fired = true;
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
  } else {
    if (g_consecutive_fetch_failures < 255) {
      ++g_consecutive_fetch_failures;
    }
    ui::radarDisplaySetFetchFailures(g_consecutive_fetch_failures);
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
  services::adsb::setPollFn(wifiLoop);
  services::adsb::fetchInit();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();
  handleAsyncFetchResult();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
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
