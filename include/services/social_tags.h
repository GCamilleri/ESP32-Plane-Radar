#pragma once

#include <cstddef>
#include <cstdint>

#include "services/adsb_client.h"

namespace services::social {

// Device identity and the claim/release queue for social aircraft tags.
//
// This module owns no socket. Requests to the Worker travel on the same keep-alive
// connection adsb_client already holds for the feed, because both go to the same
// host. That is deliberate: a second concurrent TLS session would put another
// ~32 KB of mbedTLS buffers on a board where adsb_client.cpp already documents
// handshakes failing on a fragmented heap.
//
// So the flow is: the UI queues a request, the fetch task drains it via
// nextRequest()/completeRequest() straight after the feed GET, and the UI reads
// the outcome back through pendingState().

/** Longest signed request this module emits, including the terminator. */
constexpr size_t kRequestPathMax = 24;
/** Sized for the registration body, the longest of the three: dev, secret, handle. */
constexpr size_t kRequestBodyMax = 96;
constexpr size_t kDeviceIdMax = 13;   // 12 hex chars
constexpr size_t kSignatureMax = 65;  // 64 hex chars

/** Load or create the device identity. Call once in setup(), before any fetch. */
void init();

/**
 * True when tags can actually work: a proxy is configured, the user has the
 * feature switched on, and the build has somewhere to send claims.
 */
bool enabled();

/**
 * This device's handle as other radars see it, "" until registration succeeds.
 * Deliberately strict: matching tags against a handle we have not been granted
 * would mark someone else's tag as ours.
 */
const char* handle();
/** Handle the user asked for, which may not be the one the server granted. */
const char* wantedHandle();
/** Stable 12 hex char device id derived from the secret. */
const char* deviceId();
bool registered();

/** Handle chosen in the WiFi portal. Takes effect at the next registration. */
void saveHandleFromPortal(const char* value);

/**
 * The Worker's clock, learned from the PR1 header. The ESP32 has no RTC, so
 * without this every signed request would be rejected for clock skew.
 */
void noteServerEpoch(uint32_t epoch);
/** Best estimate of the current epoch, 0 if no feed response has arrived yet. */
uint32_t epochNow();

// --- Claim queue, driven by the UI ---

enum class PendingState : uint8_t {
  kIdle,
  kQueued,
  kInFlight,
  kClaimed,
  kReleased,
  kDenied,   // someone else holds it; pendingOwner() says who
  kError,
};

/** Queue a claim. Replaces any queued-but-not-sent request. */
void requestClaim(uint32_t icao);
/** Queue a release of our own tag. */
void requestRelease(uint32_t icao);

PendingState pendingState();
uint32_t pendingIcao();
/** On kDenied, the handle that currently holds the tag. Otherwise "". */
const char* pendingOwner();
/** Acknowledge a finished request so the UI stops showing its outcome. */
void clearPending();

// --- Transport hooks, called only from the fetch task ---

struct Request {
  char path[kRequestPathMax];
  char body[kRequestBodyMax];
  char device[kDeviceIdMax];
  char timestamp[12];
  char signature[kSignatureMax];
  /** Registration is unsigned (there is no shared secret yet); claims are signed. */
  bool is_signed;
};

/**
 * Fill *out with the next request to send, or return false when there is nothing
 * to do. Signs the request as a side effect, so call it immediately before sending.
 */
bool nextRequest(Request* out);

/** Report the outcome. `body` may be null. */
void completeRequest(int http_code, const char* body);

}  // namespace services::social
