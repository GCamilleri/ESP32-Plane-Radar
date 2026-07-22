#pragma once

#include <cstddef>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/** Create background fetch task. Call once in setup(). */
void fetchInit();
/** Start an async fetch. No-op if one is already running. */
void fetchStartAsync(double center_lat, double center_lon, float fetch_radius_km);
/** True while an async fetch is in progress. */
bool fetchAsyncBusy();
/** If an async fetch completed, sets *success and returns true. */
bool fetchAsyncConsumeResult(bool* success);

}  // namespace services::adsb
