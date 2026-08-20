#include "ui/aircraft_motion.h"

#include <cmath>

#include "ui/radar_geo.h"

namespace ui::motion {

namespace {

constexpr float kKmPerKnotSec = 1.852f / 3600.0f;
constexpr float kDegToRad = 0.01745329252f;
/** Floor on cos(lat) so the longitude scale cannot blow up near the poles. */
constexpr float kMinCosLat = 0.02f;

/** Deci-degrees, i.e. 3600 to the circle. */
constexpr uint16_t kDeciDegPerTurn = 3600;

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

  // Drawn position. Kept as floats: at these ranges a degree is tens of km, so
  // anything coarser would quantise the motion this file exists to smooth.
  float lat;
  float lon;

  // Attitude, drawn and wanted. Deci-degrees and whole knots, because the display
  // rounds to pixels long before it can tell the difference, and 6 bytes per
  // aircraft of heap headroom is worth more than the precision.
  uint16_t nose_dd;
  uint16_t track_dd;
  uint16_t gs_kt;
  uint16_t target_nose_dd;
  uint16_t target_track_dd;
  uint16_t target_gs_kt;

  /** Label side from the last frame, owned by radar_display. */
  uint8_t label_side;
};

Entry s_entries[services::adsb::kMaxAircraft];
/** One bit per slot, set for entries present in the snapshot being folded in. */
uint64_t s_seen = 0;
uint32_t s_last_advance_ms = 0;
bool s_have_frame_clock = false;

float clampSeconds(uint32_t elapsed_ms, float max_sec) {
  const float sec = static_cast<float>(elapsed_ms) / 1000.0f;
  return sec > max_sec ? max_sec : sec;
}

float normaliseDeg(float deg) {
  deg = fmodf(deg, 360.0f);
  return deg < 0.0f ? deg + 360.0f : deg;
}

uint16_t toDeciDeg(float deg) {
  const long dd = lroundf(normaliseDeg(deg) * 10.0f);
  return static_cast<uint16_t>(dd % kDeciDegPerTurn);
}

float fromDeciDeg(uint16_t dd) { return static_cast<float>(dd) * 0.1f; }

uint16_t toKnots(float gs) {
  if (gs <= 0.0f) return 0;
  const long kt = lroundf(gs);
  return kt > 65535 ? 65535 : static_cast<uint16_t>(kt);
}

/**
 * One filter step towards `target`, in deci-degrees, the short way round.
 *
 * Snaps when the step rounds away to nothing, or the drawn value would stall a
 * fraction of a degree short of the target and sit there.
 */
uint16_t stepAngle(uint16_t from, uint16_t target, float alpha) {
  int diff = static_cast<int>(target) - static_cast<int>(from);
  if (diff > kDeciDegPerTurn / 2) diff -= kDeciDegPerTurn;
  if (diff < -kDeciDegPerTurn / 2) diff += kDeciDegPerTurn;
  const long step = lroundf(static_cast<float>(diff) * alpha);
  if (step == 0) {
    return target;
  }
  long next = (static_cast<long>(from) + step) % kDeciDegPerTurn;
  if (next < 0) next += kDeciDegPerTurn;
  return static_cast<uint16_t>(next);
}

/** As stepAngle, for a plain magnitude. */
uint16_t stepMagnitude(uint16_t from, uint16_t target, float alpha) {
  const int diff = static_cast<int>(target) - static_cast<int>(from);
  const long step = lroundf(static_cast<float>(diff) * alpha);
  if (step == 0) {
    return target;
  }
  return static_cast<uint16_t>(static_cast<long>(from) + step);
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

size_t slotOf(const Entry* entry) {
  return static_cast<size_t>(entry - s_entries);
}

void adopt(Entry* entry, const services::adsb::Aircraft& plane, uint32_t base_ms,
           float age_sec) {
  entry->icao = plane.icao;
  entry->base_lat = plane.lat;
  entry->base_lon = plane.lon;
  entry->base_ms = base_ms;
  resolveVelocity(entry, plane.lat, plane.track_deg, plane.gs_knots);
  entry->target_nose_dd = toDeciDeg(plane.nose_deg);
  entry->target_track_dd = toDeciDeg(plane.track_deg);
  entry->target_gs_kt = toKnots(plane.gs_knots);
  // A newcomer appears where it should be now, not where it was when the block was
  // built, so there is nothing to ease in from.
  extrapolate(*entry, age_sec, &entry->lat, &entry->lon);
  entry->nose_dd = entry->target_nose_dd;
  entry->track_dd = entry->target_track_dd;
  entry->gs_kt = entry->target_gs_kt;
  entry->label_side = kNoLabelSide;
}

void refresh(Entry* entry, const services::adsb::Aircraft& plane,
             uint32_t base_ms) {
  entry->target_nose_dd = toDeciDeg(plane.nose_deg);
  entry->target_track_dd = toDeciDeg(plane.track_deg);
  entry->target_gs_kt = toKnots(plane.gs_knots);

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

  s_seen = 0;

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
      s_seen |= (1ULL << slotOf(entry));
    }
  }

  for (size_t slot = 0; slot < services::adsb::kMaxAircraft; ++slot) {
    if ((s_seen & (1ULL << slot)) == 0) {
      s_entries[slot].icao = 0;
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
    s_seen |= (1ULL << slotOf(slot));
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

    entry.lat += (target_lat - entry.lat) * alpha;
    entry.lon += (target_lon - entry.lon) * alpha;
    entry.nose_dd = stepAngle(entry.nose_dd, entry.target_nose_dd, alpha);
    entry.track_dd = stepAngle(entry.track_dd, entry.target_track_dd, alpha);
    entry.gs_kt = stepMagnitude(entry.gs_kt, entry.target_gs_kt, alpha);
  }
}

Motion stateFor(const services::adsb::Aircraft& plane) {
  if (plane.icao != 0) {
    const Entry* entry = find(plane.icao);
    if (entry != nullptr) {
      return Motion{entry->lat, entry->lon, fromDeciDeg(entry->nose_dd),
                    fromDeciDeg(entry->track_dd),
                    static_cast<float>(entry->gs_kt)};
    }
  }
  return Motion{plane.lat, plane.lon, plane.nose_deg, plane.track_deg,
                plane.gs_knots};
}

uint8_t labelSide(uint32_t icao) {
  if (icao == 0) {
    return kNoLabelSide;
  }
  const Entry* entry = find(icao);
  return entry == nullptr ? kNoLabelSide : entry->label_side;
}

void setLabelSide(uint32_t icao, uint8_t side) {
  if (icao == 0) {
    return;
  }
  Entry* entry = find(icao);
  if (entry != nullptr) {
    entry->label_side = side;
  }
}

void reset() {
  for (Entry& entry : s_entries) {
    entry.icao = 0;
    entry.label_side = kNoLabelSide;
  }
  s_seen = 0;
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
