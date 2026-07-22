#include "ui/menu.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/wifi_setup.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::menu {
namespace {

enum class State : uint8_t { kClosed, kMenu, kSetting };

struct MenuItem {
  const char* label;
  uint8_t value_count;
  const char* const* value_labels;
  uint8_t (*get_value)();
  void (*set_value)(uint8_t);
};

const char* const kRangeLabels[] = {"5 km", "10 km", "15 km", "25 km"};
const char* const kLabelModeLabels[] = {"All", "Flight", "None"};
const char* const kRunwayModeLabels[] = {"Off", "Large", "All"};
const char* const kPollRateLabels[] = {"1s", "3s", "5s", "10s"};
const char* const kSweepLabels[] = {"Off", "On"};

uint8_t getRange() { return radar::rangeIndex(); }
void setRange(uint8_t v) { radar::setRangeIndex(v); }

uint8_t getLabelMode() { return radar::labelMode(); }
void setLabelMode(uint8_t v) { radar::setLabelMode(v); }

uint8_t getRunwayMode() { return radar::runwayMode(); }
void setRunwayMode(uint8_t v) { radar::setRunwayMode(v); }

uint8_t getPollRate() { return radar::pollRateIndex(); }
void setPollRate(uint8_t v) { radar::setPollRateIndex(v); }

uint8_t getSweep() { return radar::sweepEnabled() ? 1 : 0; }
void setSweep(uint8_t v) { radar::setSweepEnabled(v == 1); }

constexpr size_t kHeadingIndex = 1;
constexpr size_t kSettingCount = 6;
constexpr size_t kResetWifiIndex = kSettingCount;
constexpr size_t kMenuItemCount = kSettingCount + 1;

const MenuItem kMenuItems[kSettingCount] = {
    {"Range", radar::kRangePresetCount, kRangeLabels, getRange, setRange},
    {"Heading", 0, nullptr, nullptr, nullptr},
    {"Labels", radar::kLabelModeCount, kLabelModeLabels,
     getLabelMode, setLabelMode},
    {"Airports", radar::kRunwayModeCount, kRunwayModeLabels,
     getRunwayMode, setRunwayMode},
    {"Poll Rate", radar::kPollRatePresetCount, kPollRateLabels,
     getPollRate, setPollRate},
    {"Sweep", 2, kSweepLabels, getSweep, setSweep},
};

const char* compassDir(uint16_t deg) {
  switch (deg) {
    case 0:   return "N";
    case 45:  return "NE";
    case 90:  return "E";
    case 135: return "SE";
    case 180: return "S";
    case 225: return "SW";
    case 270: return "W";
    case 315: return "NW";
    default:  return "";
  }
}

char s_heading_buf[12];

const char* headingLabel() {
  const uint16_t deg = radar::headingDegInt();
  const char* dir = compassDir(deg);
  if (dir[0] != '\0') {
    snprintf(s_heading_buf, sizeof(s_heading_buf), "%u%s %s", deg,
             "\xC2\xB0", dir);
  } else {
    snprintf(s_heading_buf, sizeof(s_heading_buf), "%u%s", deg, "\xC2\xB0");
  }
  return s_heading_buf;
}

State s_state = State::kClosed;
uint8_t s_cursor = 0;
unsigned long s_last_interaction_ms = 0;
bool s_short_hold_fired = false;
bool s_needs_redraw = true;

uint16_t s_color_bg = 0;
uint16_t s_color_ring = 0;
uint16_t s_color_selected = 0;
uint16_t s_color_dim = 0;
uint16_t s_color_value = 0;
uint16_t s_color_hint = 0;
bool s_colors_ready = false;

void initColors() {
  if (s_colors_ready) return;
  s_color_bg = tft.color565(radar::kMenuBgR, radar::kMenuBgG, radar::kMenuBgB);
  s_color_ring = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  s_color_selected = tft.color565(radar::kMenuSelectedR, radar::kMenuSelectedG,
                                  radar::kMenuSelectedB);
  s_color_dim = tft.color565(radar::kMenuDimR, radar::kMenuDimG, radar::kMenuDimB);
  s_color_value = tft.color565(radar::kMenuValueR, radar::kMenuValueG,
                               radar::kMenuValueB);
  s_color_hint = tft.color565(radar::kMenuHintR, radar::kMenuHintG, radar::kMenuHintB);
  s_colors_ready = true;
}

void drawOuterRing() {
  for (int i = 0; i < radar::kMenuRingThickness; ++i) {
    tft.drawCircle(radar::kCenterX, radar::kCenterY,
                   radar::kMenuRingRadius - i, s_color_ring);
  }
}

void drawMenuScreen() {
  initColors();
  tft.fillScreen(s_color_bg);
  drawOuterRing();
  displayFontEnsureLoaded(tft);

  const int cx = radar::kCenterX;

  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 0.85f);
  } else {
    tft.setFont(&lgfx::v1::fonts::FreeSansBold9pt7b);
  }
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(s_color_ring, s_color_bg);
  tft.drawString("SETTINGS", cx, 42);

  constexpr int kItemStartY = 72;
  constexpr int kItemSpacing = 28;
  constexpr int kItemsPerPage = 4;
  constexpr int kDotRadius = 3;
  constexpr int kDotOffsetX = -55;

  const size_t page = s_cursor / kItemsPerPage;
  const size_t page_start = page * kItemsPerPage;
  const size_t page_end = std::min(page_start + kItemsPerPage, kMenuItemCount);
  const size_t total_pages =
      (kMenuItemCount + kItemsPerPage - 1) / kItemsPerPage;

  for (size_t i = page_start; i < page_end; ++i) {
    const int slot = static_cast<int>(i - page_start);
    const int y = kItemStartY + slot * kItemSpacing;
    const bool selected = (i == s_cursor);
    const bool is_reset = (i == kResetWifiIndex);
    const uint16_t color = is_reset
        ? (selected ? tft.color565(255, 80, 80) : s_color_dim)
        : (selected ? s_color_selected : s_color_dim);

    if (selected) {
      tft.fillSmoothCircle(cx + kDotOffsetX, y, kDotRadius,
                           is_reset ? tft.color565(255, 80, 80) : s_color_selected);
    }

    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, selected ? 0.90f : 0.82f);
    } else {
      tft.setFont(selected ? &lgfx::v1::fonts::FreeSansBold12pt7b
                           : &lgfx::v1::fonts::FreeSans9pt7b);
    }
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(color, s_color_bg);

    if (is_reset) {
      tft.drawString("Reset WiFi", cx + kDotOffsetX + 10, y);
      continue;
    }

    tft.drawString(kMenuItems[i].label, cx + kDotOffsetX + 10, y);

    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 0.76f);
    } else {
      tft.setFont(&lgfx::v1::fonts::Font2);
    }
    tft.setTextDatum(textdatum_t::middle_right);
    tft.setTextColor(selected ? s_color_dim : s_color_hint, s_color_bg);
    if (i == kHeadingIndex) {
      tft.drawString(headingLabel(), cx + 62, y);
    } else {
      const uint8_t val_idx = kMenuItems[i].get_value();
      tft.drawString(kMenuItems[i].value_labels[val_idx], cx + 62, y);
    }
  }

  // Page indicator dots
  if (total_pages > 1) {
    constexpr int kPageDotRadius = 2;
    constexpr int kPageDotGap = 10;
    const int dots_width = (static_cast<int>(total_pages) - 1) * kPageDotGap;
    const int dots_x = cx - dots_width / 2;
    constexpr int kPageDotsY = 188;
    for (size_t p = 0; p < total_pages; ++p) {
      const int dx = dots_x + static_cast<int>(p) * kPageDotGap;
      if (p == page) {
        tft.fillSmoothCircle(dx, kPageDotsY, kPageDotRadius, s_color_selected);
      } else {
        tft.drawCircle(dx, kPageDotsY, kPageDotRadius, s_color_dim);
      }
    }
  }

  // Hint at bottom
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 0.65f);
  } else {
    tft.setFont(&lgfx::v1::fonts::Font2);
  }
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(s_color_hint, s_color_bg);
  tft.drawString("tap:next  hold:select", cx, 205);
}

void drawSettingScreen() {
  initColors();
  tft.fillScreen(s_color_bg);
  drawOuterRing();
  displayFontEnsureLoaded(tft);

  const int cx = radar::kCenterX;
  const MenuItem& item = kMenuItems[s_cursor];

  // Setting name
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 0.85f);
  } else {
    tft.setFont(&lgfx::v1::fonts::FreeSansBold9pt7b);
  }
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(s_color_ring, s_color_bg);
  tft.drawString(item.label, cx, 62);

  if (s_cursor == kHeadingIndex) {
    // Heading: show degree and compass direction
    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 1.2f);
    } else {
      tft.setFont(&lgfx::v1::fonts::FreeSansBold18pt7b);
    }
    tft.setTextColor(s_color_value, s_color_bg);
    tft.drawString(headingLabel(), cx, 115);

    // Compass direction subtitle
    const char* dir = compassDir(radar::headingDegInt());
    if (dir[0] != '\0') {
      if (displayFontIsSmooth()) {
        displayFontSetSmoothSize(tft, 0.72f);
      } else {
        tft.setFont(&lgfx::v1::fonts::FreeSans9pt7b);
      }
      tft.setTextColor(s_color_selected, s_color_bg);
      tft.drawString(dir, cx, 150);
    }
  } else {
    // Generic setting: value + dot indicators
    const uint8_t val_idx = item.get_value();
    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 1.2f);
    } else {
      tft.setFont(&lgfx::v1::fonts::FreeSansBold18pt7b);
    }
    tft.setTextColor(s_color_value, s_color_bg);
    tft.drawString(item.value_labels[val_idx], cx, 120);

    constexpr int kDotRadius = 5;
    constexpr int kDotGap = 16;
    const int dot_count = item.value_count;
    const int dots_width = (dot_count - 1) * kDotGap;
    const int dots_start_x = cx - dots_width / 2;
    constexpr int kDotsY = 168;

    for (int i = 0; i < dot_count; ++i) {
      const int dx = dots_start_x + i * kDotGap;
      if (i == val_idx) {
        tft.fillSmoothCircle(dx, kDotsY, kDotRadius, s_color_selected);
      } else {
        tft.drawCircle(dx, kDotsY, kDotRadius, s_color_dim);
      }
    }
  }

  // Hint
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 0.65f);
  } else {
    tft.setFont(&lgfx::v1::fonts::Font2);
  }
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(s_color_hint, s_color_bg);
  tft.drawString("tap:change  hold:back", cx, 200);
}

void redraw() {
  if (s_state == State::kMenu) {
    drawMenuScreen();
  } else if (s_state == State::kSetting) {
    drawSettingScreen();
  }
  s_needs_redraw = false;
}

void touch() { s_last_interaction_ms = millis(); }

void close() {
  s_state = State::kClosed;
  s_needs_redraw = false;
}

}  // namespace

void init() {}

bool isOpen() { return s_state != State::kClosed; }

void open() {
  s_state = State::kMenu;
  s_cursor = 0;
  s_needs_redraw = true;
  s_short_hold_fired = true;
  touch();
}

void update() {
  if (s_state == State::kClosed) return;

  // Timeout
  if (millis() - s_last_interaction_ms >= config::kMenuTimeoutMs) {
    close();
    return;
  }

  // Consume taps instantly (no debounce -- responsiveness matters in the menu).
  while (bootButtonConsumeTap()) {
    touch();
    if (s_state == State::kMenu) {
      s_cursor = (s_cursor + 1) % kMenuItemCount;
      s_needs_redraw = true;
    } else if (s_state == State::kSetting) {
      if (s_cursor == kHeadingIndex) {
        radar::headingNext();
      } else {
        const MenuItem& item = kMenuItems[s_cursor];
        const uint8_t next = (item.get_value() + 1) % item.value_count;
        item.set_value(next);
      }
      s_needs_redraw = true;
    }
  }

  // Handle hold (1s = select/back)
  const unsigned long held_ms = bootButtonHeldMs();
  if (held_ms >= config::kBootShortHoldMs && !s_short_hold_fired) {
    s_short_hold_fired = true;
    touch();
    if (s_state == State::kMenu) {
      if (s_cursor == kResetWifiIndex) {
        close();
        wifiResetCredentialsAndReboot();
        return;
      }
      s_state = State::kSetting;
      s_needs_redraw = true;
    } else if (s_state == State::kSetting) {
      s_state = State::kMenu;
      s_needs_redraw = true;
    }
  }
  if (!bootButtonIsHeld()) {
    s_short_hold_fired = false;
  }

  if (s_needs_redraw) {
    redraw();
  }
}

}  // namespace ui::menu
