#pragma once

#include <cstddef>
#include <cstdint>

#include "services/adsb_client.h"

// Smoothed aircraft positions between feed polls.
//
// The feed lands every 3-10 s, so drawing reported positions straight from the
// array makes every aircraft sit still and then jump: about 15 px per poll on the
// 5 km range at airliner speeds. This keeps a small table keyed by ICAO, dead
// reckons each aircraft forward from its last reported fix along its own track,
// and eases the drawn position onto that extrapolation, so a correction from the
// server arrives as a slide rather than a jump.
//
// No I/O and no clock of its own: every entry point is given the current millis(),
// which is what makes the whole thing host-testable.

namespace ui::motion {

/** Correction filter time constant, seconds: ~63% of the error is taken out per tau. */
constexpr float kCorrectionTauSec = 0.4f;

/**
 * Longest a fix is dead reckoned before the aircraft freezes.
 *
 * With a dead feed a stopped symbol beside the amber centre dot is honest; one
 * still flying on a minute-old heading is not. Extrapolation error is cumulative,
 * so there is no version of this that stays useful for long.
 */
constexpr float kMaxExtrapolationSec = 12.0f;

/** Frame gaps longer than this are treated as this, so a stall doesn't snap. */
constexpr float kMaxFrameStepSec = 0.5f;

/** What to draw for one aircraft this frame. */
struct Motion {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
};

/**
 * Fold a freshly parsed feed response into the table.
 *
 * `position_age_ms` is how old the positions were when the response was built (the
 * server's cache age, from the PR1 header). It shifts the dead reckoning baseline
 * back, so a cached block is not mistaken for a brand new fix and the aircraft is
 * not tugged backwards when one turns up. Pass 0 when it is unknown.
 */
void onSnapshot(const services::adsb::Aircraft* planes, size_t count,
                uint32_t now_ms, uint32_t position_age_ms);

/** Advance every tracked aircraft to `now_ms`. Call once per rendered frame. */
void advance(uint32_t now_ms);

/** Smoothed state for `plane`, or its reported state when it is not tracked. */
Motion stateFor(const services::adsb::Aircraft& plane);

/**
 * Which of the four candidate sides an aircraft's label took last frame, and a
 * setter for this frame's choice. kNoLabelSide means "no previous choice".
 *
 * A display concern parked in the motion table on purpose: it needs to persist per
 * aircraft across frames and be keyed by ICAO, which is exactly what this table
 * already is. The alternative was a second 64-entry table beside it, and RAM here
 * comes out of the heap that mbedTLS needs for its handshakes.
 */
constexpr uint8_t kNoLabelSide = 0xFF;
uint8_t labelSide(uint32_t icao);
void setLabelSide(uint32_t icao, uint8_t side);

/** Drop every entry. Also resets the frame clock. */
void reset();

/** How many aircraft are being tracked. Exposed for the host tests. */
size_t trackedCount();

}  // namespace ui::motion
