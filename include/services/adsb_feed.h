#pragma once

#include <cstddef>
#include <cstdint>

#include "services/adsb_client.h"

namespace services::adsb {

// PR1: the line-oriented format served by the Cloudflare Worker proxy. Defined in
// worker/src/protocol.ts; keep the two in step.
//
//   PR1 <server_epoch> <lock_seconds> <aircraft_count> <tag_count>
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
};

struct FeedTag {
  uint32_t icao;
  char handle[kTagHandleLen];
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
