#include "ui/aircraft_trails.h"

#include <Arduino.h>
#include <cstring>

#include "services/adsb_client.h"

namespace ui::trails {
namespace {

bool s_enabled = true;
AircraftTrail s_trails[kMaxTracked];
size_t s_trail_count = 0;

AircraftTrail* findTrail(const char* callsign) {
  for (size_t i = 0; i < s_trail_count; ++i) {
    if (strncmp(s_trails[i].callsign, callsign, 8) == 0) {
      return &s_trails[i];
    }
  }
  return nullptr;
}

AircraftTrail* allocTrail(const char* callsign) {
  if (s_trail_count < kMaxTracked) {
    auto* t = &s_trails[s_trail_count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->callsign, callsign, 8);
    t->callsign[8] = '\0';
    return t;
  }
  // Evict oldest trail.
  uint32_t oldest_ms = UINT32_MAX;
  size_t oldest_idx = 0;
  for (size_t i = 0; i < s_trail_count; ++i) {
    if (s_trails[i].last_seen_ms < oldest_ms) {
      oldest_ms = s_trails[i].last_seen_ms;
      oldest_idx = i;
    }
  }
  auto* t = &s_trails[oldest_idx];
  memset(t, 0, sizeof(*t));
  strncpy(t->callsign, callsign, 8);
  t->callsign[8] = '\0';
  return t;
}

void pushPoint(AircraftTrail* t, float lat, float lon) {
  t->points[t->head] = {lat, lon};
  t->head = (t->head + 1) % kTrailLength;
  if (t->count < kTrailLength) ++t->count;
  t->last_seen_ms = millis();
}

void expireStale() {
  const uint32_t now = millis();
  constexpr uint32_t kExpireMs = 60000;  // 60 s without update
  size_t write = 0;
  for (size_t i = 0; i < s_trail_count; ++i) {
    if (now - s_trails[i].last_seen_ms < kExpireMs) {
      if (write != i) s_trails[write] = s_trails[i];
      ++write;
    }
  }
  s_trail_count = write;
}

}  // namespace

void init() {
  s_trail_count = 0;
  memset(s_trails, 0, sizeof(s_trails));
}

bool enabled() { return s_enabled; }
void setEnabled(bool e) { s_enabled = e; }

size_t trailCount() { return s_trail_count; }
const AircraftTrail* trails() { return s_trails; }

void recordPositions() {
  if (!s_enabled) return;

  expireStale();

  const size_t n = services::adsb::aircraftCount();
  const auto* planes = services::adsb::aircraftList();

  for (size_t i = 0; i < n; ++i) {
    if (planes[i].callsign[0] == '\0') continue;

    AircraftTrail* t = findTrail(planes[i].callsign);
    if (!t) t = allocTrail(planes[i].callsign);
    pushPoint(t, planes[i].lat, planes[i].lon);
  }
}

}  // namespace ui::trails
