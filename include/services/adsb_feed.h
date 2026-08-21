#pragma once

#include <cstddef>
#include <cstdint>

#include "services/adsb_client.h"

namespace services::adsb {

// PR1: the line-oriented format served by the Cloudflare Worker proxy. Defined in
// worker/src/protocol.ts; keep the two in step.
//
//   PR1 <server_epoch> <lock_seconds> <aircraft_count> <tag_count> [<pos_age_ms>]
//   A,<hex>,<lat>,<lon>,<nose>,<track>,<gs>,<callsign>,<type>,<alt>
//   T,<hex>,<handle>,<ttl_seconds>
//
// Parsing is line at a time into fixed storage so the proxy path allocates nothing
// and never holds the whole payload. That is the point of not using JSON here: on
// this board the heap is tightest during exactly the window when the response is
// arriving.

/** Longest PR1 line worth accepting, including the terminator. */
constexpr size_t kFeedLineMax = 96;

struct FeedHeader {
  uint32_t server_epoch;
  uint16_t lock_sec;
  uint16_t aircraft_count;
  uint16_t tag_count;
  /**
   * How old the positions in this response were when it was built, in ms.
   *
   * The server pools radars onto one cached fetch per map cell, so a response can
   * carry positions a few seconds old, and how old varies from poll to poll. The
   * dead reckoning in ui/aircraft_motion needs that or it tugs every aircraft
   * backwards whenever a fresher block lands. Optional field: a server that does
   * not send it leaves this 0, which behaves as it always did.
   */
  uint32_t pos_age_ms;
};

struct FeedTag {
  uint32_t icao;
  char handle[kTagHandleLen];
  /**
   * Seconds of exclusivity left, not how long the tag lives.
   *
   * A tag stays on its aircraft until its owner releases it or another radar claims
   * it, which only becomes possible once this reaches 0. Nothing on the device acts
   * on it; it is parsed so the server can say something here later.
   */
  uint16_t ttl_sec;
};

enum class FeedLine : uint8_t { kInvalid, kHeader, kAircraft, kTag };

/**
 * Parse one NUL-terminated PR1 line and fill the matching out-parameter.
 *
 * `line` is modified in place: fields are terminated where the separators were.
 * Out-parameters other than the returned kind are left untouched. Aircraft records
 * come back with the social tag fields cleared; joining tags onto aircraft is the
 * caller's job, since it needs to know this device's own handle.
 */
FeedLine feedParseLine(char* line, FeedHeader* header, Aircraft* aircraft,
                       FeedTag* tag);

/** True when the aircraft's altitude field marks it as on the ground. */
bool feedAircraftOnGround(const Aircraft& aircraft);

/**
 * Copy `handle` onto every aircraft whose ICAO matches `icao`, marking it as this
 * device's own when `own_handle` matches. Returns the number of aircraft updated.
 */
size_t feedApplyTag(Aircraft* aircraft, size_t count, const FeedTag& tag,
                    const char* own_handle);

}  // namespace services::adsb
