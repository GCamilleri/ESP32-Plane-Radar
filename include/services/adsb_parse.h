#pragma once

#include <ArduinoJson.h>

#include "services/adsb_client.h"

namespace services::adsb {

// Pure JSON -> Aircraft field parsing, split out of the HTTP client so it can be
// unit-tested on the host with in-memory JSON (no WiFi/TLS). See test/native.

/** Nose heading, preferring true_heading > mag_heading > track > dir. 0 if none. */
float pickNoseHeading(const JsonObject& plane);
/** Track heading, preferring track > true_heading > mag_heading > dir. 0 if none. */
float pickTrackHeading(const JsonObject& plane);
/** Ground speed (knots), preferring gs > tas > ias. 0 if none. */
float pickGroundSpeed(const JsonObject& plane);

/** True when alt_baro is the string "ground". */
bool isOnGround(const JsonObject& plane);

/** Fill callsign (flight, else hex), type, and altitude tag from the JSON. */
void fillTagFields(Aircraft* ac, const JsonObject& plane);

}  // namespace services::adsb
