#include "ui/target_select.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/adsb_client.h"
#include "services/social_tags.h"
#include "services/wifi_setup.h"
#include "ui/radar_geo.h"
#include "ui/radar_theme.h"

namespace ui::target {
namespace {

bool s_open = false;
/**
 * Selection is held by ICAO, not by index: the aircraft array is rebuilt from
 * scratch on every poll, so an index would silently start pointing at a different
 * aeroplane mid-gesture.
 */
uint32_t s_selected_icao = 0;
unsigned long s_last_interaction_ms = 0;
bool s_hold_fired = false;
char s_status[32];

float distanceKm(const services::adsb::Aircraft& plane) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  geo::offsetKmFromCenter(plane.lat, plane.lon, &dx_km, &dy_km, &dist_km);
  return dist_km;
}

/** Index of the nearest aircraft, or -1 when the radar is empty. */
int nearestIndex() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  int best = -1;
  float best_km = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float km = distanceKm(planes[i]);
    if (best < 0 || km < best_km) {
      best = static_cast<int>(i);
      best_km = km;
    }
  }
  return best;
}

/**
 * Next aircraft outward from the one currently selected, wrapping to the nearest.
 * Ordering by distance keeps the cursor's movement predictable on a display where
 * "next" has no natural meaning.
 */
int nextIndexOutward(float from_km) {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  int best = -1;
  float best_km = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float km = distanceKm(planes[i]);
    if (km <= from_km) {
      continue;
    }
    if (best < 0 || km < best_km) {
      best = static_cast<int>(i);
      best_km = km;
    }
  }
  return best >= 0 ? best : nearestIndex();
}

void touch() { s_last_interaction_ms = millis(); }

void setStatusFromPlane(const services::adsb::Aircraft& plane) {
  if (plane.tag_handle[0] != '\0') {
    // Same sigil as the label on the radar, so the footer and the tag read as
    // the same thing rather than two different codes.
    snprintf(s_status, sizeof(s_status), "%s  %s%s", plane.callsign,
             radar::kTagHandleSigil, plane.tag_handle);
  } else {
    snprintf(s_status, sizeof(s_status), "%s", plane.callsign);
  }
}

void refreshStatus() {
  const int index = selectedIndex();
  if (index < 0) {
    std::strncpy(s_status, "no targets", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    return;
  }
  setStatusFromPlane(services::adsb::aircraftList()[index]);
}

void selectIndex(int index) {
  if (index < 0) {
    s_selected_icao = 0;
    refreshStatus();
    return;
  }
  s_selected_icao = services::adsb::aircraftList()[index].icao;
  refreshStatus();
}

/** Apply the outcome of a claim the fetch task has since completed. */
void pollPendingResult() {
  using services::social::PendingState;
  switch (services::social::pendingState()) {
    case PendingState::kQueued:
    case PendingState::kInFlight:
      std::strncpy(s_status, "tagging...", sizeof(s_status) - 1);
      s_status[sizeof(s_status) - 1] = '\0';
      break;
    case PendingState::kClaimed:
      snprintf(s_status, sizeof(s_status), "tagged as %s%s",
               radar::kTagHandleSigil, services::social::handle());
      services::social::clearPending();
      break;
    case PendingState::kReleased:
      std::strncpy(s_status, "tag released", sizeof(s_status) - 1);
      s_status[sizeof(s_status) - 1] = '\0';
      services::social::clearPending();
      break;
    case PendingState::kClearedAll:
      // Fired from the menu, which closes itself, so this is usually only seen if
      // the picker is opened again before the state is acknowledged.
      std::strncpy(s_status, "tags cleared", sizeof(s_status) - 1);
      s_status[sizeof(s_status) - 1] = '\0';
      services::social::clearPending();
      break;
    case PendingState::kDenied:
      snprintf(s_status, sizeof(s_status), "held by %s%s",
               radar::kTagHandleSigil, services::social::pendingOwner());
      services::social::clearPending();
      break;
    case PendingState::kError: {
      // Distinguish "we could not reach the server" from "the server said no";
      // the first is worth retrying, the second is not.
      const char* reason = services::adsb::lastFeedSource() ==
                                   services::adsb::FeedSource::kProxy
                               ? "tag failed"
                               : "server offline";
      std::strncpy(s_status, reason, sizeof(s_status) - 1);
      s_status[sizeof(s_status) - 1] = '\0';
      services::social::clearPending();
      break;
    }
    case PendingState::kIdle:
      break;
  }
}

void onHold() {
  const int index = selectedIndex();
  if (index < 0) {
    return;
  }
  const services::adsb::Aircraft& plane = services::adsb::aircraftList()[index];

  if (!services::social::enabled()) {
    std::strncpy(s_status, "tags off", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    return;
  }
  if (plane.tag_handle[0] != '\0' && !plane.tag_is_mine) {
    // The server would reject this anyway; saying so locally saves a round trip
    // and reads as a deliberate rule rather than a failure.
    snprintf(s_status, sizeof(s_status), "held by %s%s",
             radar::kTagHandleSigil, plane.tag_handle);
    return;
  }

  if (plane.tag_is_mine) {
    services::social::requestRelease(plane.icao);
  } else {
    services::social::requestClaim(plane.icao);
  }
  std::strncpy(s_status, "tagging...", sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = '\0';
}

}  // namespace

bool isOpen() { return s_open; }

bool open() {
  const int nearest = nearestIndex();
  if (nearest < 0) {
    return false;
  }
  s_open = true;
  s_hold_fired = true;  // ignore a hold that is already in progress
  selectIndex(nearest);
  touch();
  return true;
}

void close() {
  s_open = false;
  s_selected_icao = 0;
  s_status[0] = '\0';
}

int selectedIndex() {
  if (!s_open || s_selected_icao == 0) {
    return -1;
  }
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  for (size_t i = 0; i < n; ++i) {
    if (planes[i].icao == s_selected_icao) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

uint32_t selectedIcao() { return s_open ? s_selected_icao : 0; }

const char* statusText() { return s_open ? s_status : ""; }

void update() {
  if (!s_open) {
    return;
  }

  // Don't close on the idle timer while a claim is outstanding: the picker is the
  // only place its result is shown, so closing would hide the answer the user is
  // waiting for. social_tags gives up on its own after
  // kSocialRequestTimeoutMs, so this cannot stay open indefinitely.
  const services::social::PendingState pending = services::social::pendingState();
  const bool awaiting_claim =
      pending == services::social::PendingState::kQueued ||
      pending == services::social::PendingState::kInFlight;

  if (!awaiting_claim &&
      millis() - s_last_interaction_ms >= config::kTargetSelectTimeoutMs) {
    close();
    return;
  }

  while (bootButtonConsumeTap()) {
    touch();
    const int current = selectedIndex();
    const float from_km =
        current >= 0 ? distanceKm(services::adsb::aircraftList()[current])
                     : -1.0f;
    selectIndex(nextIndexOutward(from_km));
  }

  if (bootButtonHeldMs() >= config::kBootShortHoldMs && !s_hold_fired) {
    s_hold_fired = true;
    touch();
    onHold();
  }
  if (!bootButtonIsHeld()) {
    s_hold_fired = false;
  }

  // The selected aircraft can drop out of the feed, and a claim completes on the
  // fetch task, so both are re-checked every iteration rather than on input.
  if (selectedIndex() < 0 && s_selected_icao != 0) {
    selectIndex(nearestIndex());
  }
  pollPendingResult();
}

}  // namespace ui::target
