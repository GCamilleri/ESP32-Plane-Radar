#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsHeadingKey[] = "heading";
constexpr char kPrefsLabelModeKey[] = "labels";
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
uint16_t s_heading_deg = 0;
uint8_t s_label_mode = 0;

template <typename T>
void nvsPut(const char* ns, const char* key, T value);

template <>
void nvsPut<uint8_t>(const char* ns, const char* key, uint8_t value) {
  Preferences prefs;
  if (prefs.begin(ns, false)) {
    prefs.putUChar(key, value);
    prefs.end();
  }
}

template <>
void nvsPut<bool>(const char* ns, const char* key, bool value) {
  Preferences prefs;
  if (prefs.begin(ns, false)) {
    prefs.putBool(key, value);
    prefs.end();
  }
}

void saveRangeIndex() { nvsPut<uint8_t>(kPrefsNamespace, kPrefsRangeKey, s_range_index); }
void saveUseMiles()   { nvsPut<bool>(kPrefsNamespace, kPrefsMilesKey, s_use_miles); }
void saveShowRunways(){ nvsPut<bool>(kPrefsNamespace, kPrefsRunwaysKey, s_show_runways); }

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);

  const uint16_t heading = s_prefs.getUShort(kPrefsHeadingKey, 0);
  s_heading_deg = (heading < 360) ? heading : 0;

  const uint8_t labels = s_prefs.getUChar(kPrefsLabelModeKey, 0);
  s_label_mode = (labels < kLabelModeCount) ? labels : 0;

  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = false;
  s_show_runways = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.end();
  }
}

uint8_t rangeIndex() { return s_range_index; }

void setRangeIndex(uint8_t idx) {
  if (idx >= kRangePresetCount) return;
  s_range_index = idx;
  saveRangeIndex();
}

uint16_t headingDegInt() { return s_heading_deg; }

float headingDeg() { return static_cast<float>(s_heading_deg); }

void headingNext() {
  s_heading_deg = (s_heading_deg + kHeadingStepDeg) % 360;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUShort(kPrefsHeadingKey, s_heading_deg);
    prefs.end();
  }
}

void headingSet(uint16_t deg) {
  s_heading_deg = deg % 360;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUShort(kPrefsHeadingKey, s_heading_deg);
    prefs.end();
  }
}

uint8_t labelMode() { return s_label_mode; }

void setLabelMode(uint8_t mode) {
  if (mode >= kLabelModeCount) return;
  s_label_mode = mode;
  nvsPut<uint8_t>(kPrefsNamespace, kPrefsLabelModeKey, mode);
}

}  // namespace ui::radar
