#include "services/adsb_feed.h"

#include <cstdlib>
#include <cstring>

#include "data/military_ranges.h"

namespace services::adsb {

namespace {

/**
 * Consume the next comma-separated field, terminating it in place. Returns nullptr
 * once the line is exhausted, which is how the callers detect a short line.
 */
char* nextField(char** cursor) {
  char* start = *cursor;
  if (start == nullptr) {
    return nullptr;
  }
  char* comma = std::strchr(start, ',');
  if (comma != nullptr) {
    *comma = '\0';
    *cursor = comma + 1;
  } else {
    *cursor = nullptr;
  }
  return start;
}

/** Consume the next space-separated field. Same contract as nextField. */
char* nextSpaceField(char** cursor) {
  char* start = *cursor;
  if (start == nullptr) {
    return nullptr;
  }
  char* space = std::strchr(start, ' ');
  if (space != nullptr) {
    *space = '\0';
    *cursor = space + 1;
  } else {
    *cursor = nullptr;
  }
  return start;
}

void copyField(char* dst, size_t dst_len, const char* src) {
  if (dst_len == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  const size_t n = strnlen(src, dst_len - 1);
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

/** Six hex digits, no more and no less: anything else is a corrupt line. */
bool parseIcao(const char* field, uint32_t* out) {
  if (field == nullptr || std::strlen(field) != 6) {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(field, &end, 16);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

float parseFloatField(const char* field) {
  return field == nullptr ? 0.0f : std::strtof(field, nullptr);
}

unsigned long parseUlongField(const char* field) {
  return field == nullptr ? 0UL : std::strtoul(field, nullptr, 10);
}

FeedLine parseHeader(char* cursor, FeedHeader* header) {
  if (header == nullptr) {
    return FeedLine::kInvalid;
  }
  const char* epoch = nextSpaceField(&cursor);
  const char* lock = nextSpaceField(&cursor);
  const char* ac_count = nextSpaceField(&cursor);
  const char* tag_count = nextSpaceField(&cursor);
  if (epoch == nullptr || lock == nullptr || ac_count == nullptr ||
      tag_count == nullptr) {
    return FeedLine::kInvalid;
  }
  header->server_epoch = static_cast<uint32_t>(parseUlongField(epoch));
  header->lock_sec = static_cast<uint16_t>(parseUlongField(lock));
  header->aircraft_count = static_cast<uint16_t>(parseUlongField(ac_count));
  header->tag_count = static_cast<uint16_t>(parseUlongField(tag_count));
  return FeedLine::kHeader;
}

FeedLine parseAircraft(char* cursor, Aircraft* aircraft) {
  if (aircraft == nullptr) {
    return FeedLine::kInvalid;
  }

  const char* hex = nextField(&cursor);
  const char* lat = nextField(&cursor);
  const char* lon = nextField(&cursor);
  const char* nose = nextField(&cursor);
  const char* track = nextField(&cursor);
  const char* gs = nextField(&cursor);
  const char* callsign = nextField(&cursor);
  const char* type = nextField(&cursor);
  const char* alt = nextField(&cursor);

  // lat/lon are the only fields the radar cannot draw without, but a line missing
  // any field means the payload is truncated or malformed, so reject the lot.
  if (alt == nullptr) {
    return FeedLine::kInvalid;
  }
  uint32_t icao = 0;
  if (!parseIcao(hex, &icao)) {
    return FeedLine::kInvalid;
  }

  aircraft->icao = icao;
  aircraft->lat = parseFloatField(lat);
  aircraft->lon = parseFloatField(lon);
  aircraft->nose_deg = parseFloatField(nose);
  aircraft->track_deg = parseFloatField(track);
  aircraft->gs_knots = parseFloatField(gs);
  copyField(aircraft->callsign, sizeof(aircraft->callsign), callsign);
  copyField(aircraft->type, sizeof(aircraft->type), type);
  copyField(aircraft->alt, sizeof(aircraft->alt), alt);
  // The military hex table stays on the device: it is small, and the direct
  // fallback path needs it anyway, so having the proxy send the flag would just
  // create two sources of truth.
  aircraft->is_military = data::military::isMilitary(icao);
  aircraft->tag_handle[0] = '\0';
  aircraft->tag_is_mine = false;

  if (aircraft->callsign[0] == '\0') {
    copyField(aircraft->callsign, sizeof(aircraft->callsign), hex);
  }
  return FeedLine::kAircraft;
}

FeedLine parseTag(char* cursor, FeedTag* tag) {
  if (tag == nullptr) {
    return FeedLine::kInvalid;
  }
  const char* hex = nextField(&cursor);
  const char* handle = nextField(&cursor);
  const char* ttl = nextField(&cursor);
  if (ttl == nullptr) {
    return FeedLine::kInvalid;
  }
  uint32_t icao = 0;
  if (!parseIcao(hex, &icao)) {
    return FeedLine::kInvalid;
  }
  if (handle[0] == '\0') {
    return FeedLine::kInvalid;
  }
  tag->icao = icao;
  copyField(tag->handle, sizeof(tag->handle), handle);
  tag->ttl_sec = static_cast<uint16_t>(parseUlongField(ttl));
  return FeedLine::kTag;
}

}  // namespace

FeedLine feedParseLine(char* line, FeedHeader* header, Aircraft* aircraft,
                       FeedTag* tag) {
  if (line == nullptr) {
    return FeedLine::kInvalid;
  }

  // Tolerate CRLF and stray trailing whitespace from any intermediary.
  size_t len = std::strlen(line);
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                     line[len - 1] == ' ')) {
    line[--len] = '\0';
  }
  if (len == 0) {
    return FeedLine::kInvalid;
  }

  if (std::strncmp(line, "PR1 ", 4) == 0) {
    return parseHeader(line + 4, header);
  }
  if (line[0] == 'A' && line[1] == ',') {
    return parseAircraft(line + 2, aircraft);
  }
  if (line[0] == 'T' && line[1] == ',') {
    return parseTag(line + 2, tag);
  }
  return FeedLine::kInvalid;
}

bool feedAircraftOnGround(const Aircraft& aircraft) {
  return std::strcmp(aircraft.alt, "GND") == 0;
}

size_t feedApplyTag(Aircraft* aircraft, size_t count, const FeedTag& tag,
                    const char* own_handle) {
  if (aircraft == nullptr) {
    return 0;
  }
  const bool mine = own_handle != nullptr && own_handle[0] != '\0' &&
                    std::strcmp(own_handle, tag.handle) == 0;
  size_t applied = 0;
  for (size_t i = 0; i < count; ++i) {
    if (aircraft[i].icao != tag.icao) {
      continue;
    }
    copyField(aircraft[i].tag_handle, sizeof(aircraft[i].tag_handle), tag.handle);
    aircraft[i].tag_is_mine = mine;
    ++applied;
  }
  return applied;
}

}  // namespace services::adsb
