#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

/** Length of a social tag handle, plus the terminator. */
constexpr size_t kTagHandleLen = 5;

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
  /** ICAO 24-bit address. Needed to join social tags onto the feed. */
  uint32_t icao;
  /** Handle of the device holding a social tag on this aircraft, "" if untagged. */
  char tag_handle[kTagHandleLen];
  /** True when tag_handle matches this device's own handle. */
  bool tag_is_mine;
  bool is_military;
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/**
 * Which upstream the last completed fetch actually used. The device prefers the
 * proxy when social features are on, but falls back to adsb.fi on its own so a
 * dead or unreachable feed server never takes the radar down with it.
 */
enum class FeedSource : uint8_t { kDirect, kProxy };
FeedSource lastFeedSource();

/**
 * How old the positions in the last completed fetch were when the response was
 * built, in ms. The feed server reports its cache age; the direct adsb.fi path has
 * nothing to report, so it is 0 there.
 */
uint32_t lastPositionAgeMs();
/** True while the proxy is being skipped after repeated failures. */
bool proxyBackedOff();
/**
 * Cancel any backoff so the next poll tries the proxy again. Called when the user
 * asks to tag something: deliberate intent should not have to wait out a timer
 * started by an unrelated failure.
 */
void retryProxyNow();

/** Fetch aircraft within fetch_radius_km of center_lat/lon. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/** Starts the background fetch task. False means no fetches will run. */
bool fetchInit();
/** Start an async fetch. No-op if one is already running. */
void fetchStartAsync(double center_lat, double center_lon, float fetch_radius_km);
/** True while an async fetch is in progress. */
bool fetchAsyncBusy();
/** If an async fetch completed, sets *success and returns true. */
bool fetchAsyncConsumeResult(bool* success);

/**
 * Close the persistent TLS/HTTP connection and force a fresh handshake on the
 * next fetch. Call after a WiFi drop so a dead keep-alive socket is not reused.
 * No-op (and safe) while an async fetch is in progress.
 */
void resetConnection();

}  // namespace services::adsb
