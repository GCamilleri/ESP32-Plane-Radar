#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::trails {

constexpr size_t kMaxTracked = 48;
constexpr size_t kTrailLength = 8;

struct TrailPoint {
  int16_t x, y;
};

struct AircraftTrail {
  char callsign[9];
  TrailPoint points[kTrailLength];
  uint8_t count;
  uint8_t head;
  uint32_t last_seen_ms;
};

/** Clear all trail history. */
void init();

/** Record current aircraft screen positions into trail history. */
void recordPositions();

void setEnabled(bool enabled);
bool enabled();

size_t trailCount();
const AircraftTrail* trails();

}  // namespace ui::trails
