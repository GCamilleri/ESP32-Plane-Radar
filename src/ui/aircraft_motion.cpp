#include "ui/aircraft_motion.h"

#include <cmath>

#include "ui/radar_geo.h"

namespace ui::motion {

namespace {

constexpr float kKmPerKnotSec = 1.852f / 3600.0f;
constexpr float kDegToRad = 0.01745329252f;
/** Floor on cos(lat) so the longitude scale cannot blow up near the poles. */
constexpr float kMinCosLat = 0.02f;

struct Entry {
  /** 0 marks a free slot. Aircraft with no ICAO cannot be tracked at all. */
  uint32_t icao;

  // Baseline: the last *distinct* reported fix, and when it was taken.
  float base_lat;
  float base_lon;
  uint32_t base_ms;
  // Velocity in degrees per second, resolved once per fix so the per-frame step is
  // two multiplies. The C3 has no FPU, and this runs 64 times per frame.
  float vlat_deg_s;
  float vlon_deg_s;

  // Where the feed says it is pointing and how fast, i.e. what we ease towards.
  float target_nose;
  float target_track;
  float target_gs;

  // What is actually drawn.
  Motion drawn;

  bool seen;
};

Entry s_entries[services::adsb::kMaxAircraft];
uint32_t s_last_advance_ms = 0;
bool s_have_frame_clock = false;

float clampSeconds(uint32_t elapsed_ms, float max_sec) {
  const float sec = static_cast<float>(elapsed_ms) / 1000.0f;
  return sec > max_sec ? max_sec : sec;
}

/** Signed shortest way round from `from` to `to`, in degrees. */
float angleErrorDeg(float from, float to) {
  float diff = to - from;
  while (diff > 180.0f) diff -= 360.0f;
  while (diff < -180.0f) diff += 360.0f;
  return diff;
}

float normaliseDeg(float deg) {
  deg = fmodf(deg, 360.0f);
  return deg < 0.0f ? deg + 360.0f : deg;
}

void resolveVelocity(Entry* entry, float lat, float track_deg, float gs_knots) {
  if (gs_knots <= 0.0f) {
    entry->vlat_deg_s = 0.0f;
    entry->vlon_deg_s = 0.0f;
    return;
  }
  const float km_per_sec = gs_knots * kKmPerKnotSec;
  const float track_rad = track_deg * kDegToRad;
  float cos_lat = cosf(lat * kDegToRad);
  if (cos_lat < kMinCosLat) {
    cos_lat = kMinCosLat;
  }
  entry->vlat_deg_s = km_per_sec * cosf(track_rad) / geo::kKmPerDeg;
  entry->vlon_deg_s = km_per_sec * sinf(track_rad) / (geo::kKmPerDeg * cos_lat);
}

/** Where the baseline says the aircraft is `age_sec` after its fix. */
void extrapolate(const Entry& entry, float age_sec, float* lat, float* lon) {
  *lat = entry.base_lat + entry.vlat_deg_s * age_sec;
  *lon = entry.base_lon + entry.vlon_deg_s * age_sec;
}

Entry* find(uint32_t icao) {
  for (Entry& entry : s_entries) {
    if (entry.icao == icao) {
      return &entry;
    }
  }
  return nullptr;
}

void adopt(Entry* entry, const services::adsb::Aircraft& plane, uint32_t base_ms,
           float age_sec) {
  entry->icao = plane.icao;
  entry->base_lat = plane.lat;
  entry->base_lon = plane.lon;
  entry->base_ms = base_ms;
  resolveVelocity(entry, plane.lat, plane.track_deg, plane.gs_knots);
  entry->target_nose = plane.nose_deg;
  entry->target_track = plane.track_deg;
  entry->target_gs = plane.gs_knots;
  // A newcomer appears where it should be now, not where it was when the block was
  // built, so there is nothing to ease in from.
  extrapolate(*entry, age_sec, &entry->drawn.lat, &entry->drawn.lon);
  entry->drawn.nose_deg = plane.nose_deg;
  entry->drawn.track_deg = plane.track_deg;
  entry->drawn.gs_knots = plane.gs_knots;
}

void refresh(Entry* entry, const services::adsb::Aircraft& plane,
             uint32_t base_ms) {
  entry->target_nose = plane.nose_deg;
  entry->target_track = plane.track_deg;
  entry->target_gs = plane.gs_knots;

  // An unchanged fix is not a new one. The server caches cells for a few seconds
  // and several radars share them, so the same coordinates come back repeatedly;
  // restamping them would drag the aircraft back to the same spot every poll and
  // undo the extrapolation. Exact comparison is the right test: identical bytes on
  // the wire parse to identical floats.
  if (plane.lat == entry->base_lat && plane.lon == entry->base_lon) {
    return;
  }
  entry->base_lat = plane.lat;
  entry->base_lon = plane.lon;
  entry->base_ms = base_ms;
  resolveVelocity(entry, plane.lat, plane.track_deg, plane.gs_knots);
}

}  // namespace

void onSnapshot(const services::adsb::Aircraft* planes, size_t count,
                uint32_t now_ms, uint32_t position_age_ms) {
  if (planes == nullptr) {
    return;
  }

  const float age_sec = clampSeconds(position_age_ms, kMaxExtrapolationSec);
  const uint32_t base_ms =
      now_ms - static_cast<uint32_t>(age_sec * 1000.0f + 0.5f);

  for (Entry& entry : s_entries) {
    entry.seen = false;
  }

  // Matching before freeing, and freeing before allocating: the table is the same
  // size as the array the feed fills, so a snapshot of entirely new aircraft only
  // fits if the departed ones have already been let go.
  for (size_t i = 0; i < count; ++i) {
    if (planes[i].icao == 0) {
      continue;
    }
    Entry* entry = find(planes[i].icao);
    if (entry != nullptr) {
      refresh(entry, planes[i], base_ms);
      entry->seen = true;
    }
  }

  for (Entry& entry : s_entries) {
    if (!entry.seen) {
      entry.icao = 0;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    if (planes[i].icao == 0) {
      continue;
    }
    if (find(planes[i].icao) != nullptr) {
      continue;
    }
    Entry* slot = find(0);
    if (slot == nullptr) {
      return;  // table full: the rest render from their reported positions
    }
    adopt(slot, planes[i], base_ms, age_sec);
    slot->seen = true;
  }
}

void advance(uint32_t now_ms) {
  const uint32_t elapsed_ms = now_ms - s_last_advance_ms;
  s_last_advance_ms = now_ms;
  if (!s_have_frame_clock) {
    // First frame after a reset has no interval to work with, so nothing moves.
    s_have_frame_clock = true;
    return;
  }

  const float dt = clampSeconds(elapsed_ms, kMaxFrameStepSec);
  if (dt <= 0.0f) {
    return;
  }
  // dt/(tau+dt) rather than 1-exp(-dt/tau): within a couple of percent over the
  // frame intervals this sees, stable for any dt, and no expf on an FPU-less core.
  const float alpha = dt / (kCorrectionTauSec + dt);

  for (Entry& entry : s_entries) {
    if (entry.icao == 0) {
      continue;
    }

    const float age_sec =
        clampSeconds(now_ms - entry.base_ms, kMaxExtrapolationSec);
    float target_lat = 0.0f;
    float target_lon = 0.0f;
    extrapolate(entry, age_sec, &target_lat, &target_lon);

    entry.drawn.lat += (target_lat - entry.drawn.lat) * alpha;
    entry.drawn.lon += (target_lon - entry.drawn.lon) * alpha;
    entry.drawn.gs_knots += (entry.target_gs - entry.drawn.gs_knots) * alpha;
    entry.drawn.nose_deg = normaliseDeg(
        entry.drawn.nose_deg +
        angleErrorDeg(entry.drawn.nose_deg, entry.target_nose) * alpha);
    entry.drawn.track_deg = normaliseDeg(
        entry.drawn.track_deg +
        angleErrorDeg(entry.drawn.track_deg, entry.target_track) * alpha);
  }
}

Motion stateFor(const services::adsb::Aircraft& plane) {
  if (plane.icao != 0) {
    const Entry* entry = find(plane.icao);
    if (entry != nullptr) {
      return entry->drawn;
    }
  }
  return Motion{plane.lat, plane.lon, plane.nose_deg, plane.track_deg,
                plane.gs_knots};
}

void reset() {
  for (Entry& entry : s_entries) {
    entry.icao = 0;
    entry.seen = false;
  }
  s_have_frame_clock = false;
  s_last_advance_ms = 0;
}

size_t trackedCount() {
  size_t n = 0;
  for (const Entry& entry : s_entries) {
    if (entry.icao != 0) {
      ++n;
    }
  }
  return n;
}

}  // namespace ui::motion
