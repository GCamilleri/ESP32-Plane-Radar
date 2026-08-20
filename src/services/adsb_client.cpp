#include "services/adsb_client.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "data/military_ranges.h"
#include "services/adsb_feed.h"
#include "services/adsb_parse.h"
#include "services/social_tags.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

// Persistent connection -- avoids a full handshake every poll cycle.
//
// Two transports, chosen per URL by scheme. adsb.fi is always TLS, but the
// self-hosted feed server is commonly plain http:// on a LAN address, which a
// WiFiClientSecure cannot speak to. Only one is ever connected at a time.
WiFiClientSecure s_tls_client;
WiFiClient s_plain_client;
HTTPClient s_http;
bool s_http_initialized = false;
/** Transport backing the open keep-alive connection, null when there is none. */
WiFiClient* s_active_client = nullptr;

// ArduinoJson filter -- parse only the fields we actually consume.
JsonDocument s_json_filter;
bool s_filter_initialized = false;

void initJsonFilter() {
  if (s_filter_initialized) {
    return;
  }
  JsonObject ac_filter = s_json_filter["ac"][0].to<JsonObject>();
  ac_filter["lat"] = true;
  ac_filter["lon"] = true;
  ac_filter["flight"] = true;
  ac_filter["hex"] = true;
  ac_filter["t"] = true;
  ac_filter["alt_baro"] = true;
  ac_filter["alt_geom"] = true;
  ac_filter["gs"] = true;
  ac_filter["tas"] = true;
  ac_filter["ias"] = true;
  ac_filter["track"] = true;
  ac_filter["true_heading"] = true;
  ac_filter["mag_heading"] = true;
  ac_filter["dir"] = true;
  s_filter_initialized = true;
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  int delay_ms = 100;
  for (int attempt = 0; attempt < 5; ++attempt) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(delay_ms);
    delay_ms = std::min(delay_ms * 2, 1000);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

// Buffered, poll-interleaved, deadline-bounded reader that feeds ArduinoJson
// directly from the TLS socket. Avoids allocating a full-payload String: the
// parser pulls bytes through a small 512-byte staging buffer while we keep the
// portal/mDNS/watchdog fed and enforce a wall-clock deadline. This removes the
// transient heap spike (up to ~100 KB in busy airspace) during the exact window
// when mbedTLS buffers make the heap tightest.
class StreamJsonReader {
 public:
  StreamJsonReader(WiFiClient* stream, int content_length,
                   unsigned long deadline_ms)
      : stream_(stream),
        remaining_(content_length),
        have_len_(content_length > 0),
        deadline_(deadline_ms) {}

  // ArduinoJson custom-reader interface: one byte or -1 at EOF. The JSON parser
  // only ever calls read(); readBytes() below is provided for completeness.
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buf_[pos_++]);
  }

  size_t readBytes(char* dst, size_t length) {
    size_t got = 0;
    while (got < length) {
      if (pos_ >= len_ && !refill()) {
        break;
      }
      dst[got++] = buf_[pos_++];
    }
    return got;
  }

  size_t bytesConsumed() const { return consumed_; }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    for (;;) {
      if (have_len_ && remaining_ <= 0) {
        return false;  // whole body consumed; clean EOF for keep-alive
      }
      pollNetwork();  // no-op in the async task; keeps portal alive if inline
      int available = stream_->available();
      if (available > 0) {
        int to_read = available > static_cast<int>(sizeof(buf_))
                          ? static_cast<int>(sizeof(buf_))
                          : available;
        if (have_len_ && to_read > remaining_) {
          to_read = remaining_;
        }
        int n = stream_->read(reinterpret_cast<uint8_t*>(buf_), to_read);
        if (n > 0) {
          len_ = n;
          consumed_ += static_cast<size_t>(n);
          if (have_len_) {
            remaining_ -= n;
          }
          return true;
        }
      }
      if (!stream_->connected() && stream_->available() <= 0) {
        return false;  // socket closed with no more data
      }
      if (static_cast<long>(millis() - deadline_) >= 0) {
        return false;  // deadline hit: parser sees truncated input, errors out
      }
      delay(1);  // yield: feed the idle task, let the render loop run
    }
  }

  WiFiClient* stream_;
  int remaining_;
  bool have_len_;
  unsigned long deadline_;
  char buf_[512];
  int pos_ = 0;
  int len_ = 0;
  size_t consumed_ = 0;
};

// Drain any bytes the parser left unread (the adsb.fi body ends with a trailing
// '\n' after the final '}') so the reused keep-alive socket starts the next
// response cleanly rather than desyncing the following status line.
void drainRemainder(WiFiClient* stream, int content_length, size_t consumed,
                    unsigned long deadline_ms) {
  if (content_length <= 0) {
    return;
  }
  uint8_t sink[128];
  while (static_cast<int>(consumed) < content_length &&
         static_cast<long>(millis() - deadline_ms) < 0) {
    pollNetwork();
    int available = stream->available();
    if (available > 0) {
      int to_read = available > static_cast<int>(sizeof(sink))
                        ? static_cast<int>(sizeof(sink))
                        : available;
      int n = stream->read(sink, to_read);
      if (n > 0) {
        consumed += static_cast<size_t>(n);
        continue;
      }
    }
    if (!stream->connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

// Line-oriented sibling of StreamJsonReader for the PR1 proxy format. Same
// discipline: a small staging buffer, the poll hook kept fed, a wall-clock
// deadline, and a clean EOF once Content-Length is consumed so the keep-alive
// socket stays reusable.
class StreamLineReader {
 public:
  StreamLineReader(WiFiClient* stream, int content_length,
                   unsigned long deadline_ms)
      : stream_(stream),
        remaining_(content_length),
        have_len_(content_length > 0),
        deadline_(deadline_ms) {}

  /** Next line into `out`, without its newline. False at EOF or deadline. */
  bool nextLine(char* out, size_t out_len) {
    size_t n = 0;
    bool any = false;
    for (;;) {
      if (pos_ >= len_ && !refill()) {
        break;
      }
      any = true;
      const char c = buf_[pos_++];
      if (c == '\n') {
        break;
      }
      if (n + 1 < out_len) {
        out[n++] = c;
      }
      // Past out_len the rest of an over-long line is dropped, which turns it
      // into a parse failure rather than a buffer overrun.
    }
    out[n] = '\0';
    return any;
  }

  size_t bytesConsumed() const { return consumed_; }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    for (;;) {
      if (have_len_ && remaining_ <= 0) {
        return false;
      }
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        int to_read = available > static_cast<int>(sizeof(buf_))
                          ? static_cast<int>(sizeof(buf_))
                          : available;
        if (have_len_ && to_read > remaining_) {
          to_read = remaining_;
        }
        const int n = stream_->read(reinterpret_cast<uint8_t*>(buf_), to_read);
        if (n > 0) {
          len_ = n;
          consumed_ += static_cast<size_t>(n);
          if (have_len_) {
            remaining_ -= n;
          }
          return true;
        }
      }
      if (!stream_->connected() && stream_->available() <= 0) {
        return false;
      }
      if (static_cast<long>(millis() - deadline_) >= 0) {
        return false;
      }
      delay(1);
    }
  }

  WiFiClient* stream_;
  int remaining_;
  bool have_len_;
  unsigned long deadline_;
  char buf_[512];
  int pos_ = 0;
  int len_ = 0;
  size_t consumed_ = 0;
};

/** Tags carried in one feed response. The server caps its own list at 64
 *  (MAX_FEED_TAGS), which must not exceed this. */
constexpr size_t kMaxFeedTags = 64;

FeedSource s_last_source = FeedSource::kDirect;
uint8_t s_proxy_failures = 0;
unsigned long s_proxy_backoff_until = 0;
/** Current backoff length, doubled on each failure and reset by a success. */
unsigned long s_proxy_backoff_ms = config::kFeedProxyBackoffBaseMs;
bool s_proxy_backed_off = false;

bool proxyConfigured() { return config::kFeedProxyBaseUrl[0] != '\0'; }

/**
 * Whether this poll should go through the feed server. The proxy is the feed whenever
 * one is configured; adsb.fi is only ever a fallback, never a mode the user picks.
 *
 * Backing off after repeated failures is what keeps the standalone promise: a
 * dead, misconfigured or over-quota proxy costs one poll cycle, then the radar
 * returns to adsb.fi on its own and retries the proxy later. The Social menu
 * toggle governs taking part in tagging, not where aircraft come from.
 */
bool shouldUseProxy() {
  if (!proxyConfigured()) {
    return false;
  }
  if (!s_proxy_backed_off) {
    return true;
  }
  if (static_cast<long>(millis() - s_proxy_backoff_until) >= 0) {
    s_proxy_backed_off = false;
    s_proxy_failures = 0;
    Serial.println("adsb: retrying the tag proxy");
    return true;
  }
  return false;
}

void noteProxyFailure() {
  if (s_proxy_failures < 255) {
    ++s_proxy_failures;
  }
  if (s_proxy_backed_off ||
      s_proxy_failures < config::kFeedProxyFailuresBeforeBackoff) {
    return;
  }

  s_proxy_backed_off = true;
  s_proxy_backoff_until = millis() + s_proxy_backoff_ms;
  Serial.printf("adsb: proxy failed %u times, using adsb.fi directly for %lus\n",
                static_cast<unsigned>(s_proxy_failures),
                s_proxy_backoff_ms / 1000UL);

  // Double for next time. Held here rather than reset on retry so a server that
  // stays down keeps stepping back instead of probing at a fixed interval.
  s_proxy_backoff_ms =
      std::min(s_proxy_backoff_ms * 2, config::kFeedProxyBackoffMaxMs);
}

/** A working proxy clears the accumulated penalty, not just the current wait. */
void noteProxySuccess() {
  s_proxy_failures = 0;
  s_proxy_backoff_ms = config::kFeedProxyBackoffBaseMs;
}

void ensureHttpClient() {
  if (s_http_initialized) {
    return;
  }
  s_tls_client.setInsecure();
  s_http.setReuse(true);
  s_http_initialized = true;
}

void dropConnection() {
  s_http.end();
  s_tls_client.stop();
  s_plain_client.stop();
  s_active_client = nullptr;
}

/**
 * Transport for `url`, with no stale socket left on it.
 *
 * Switching scheme tears the other transport down rather than leaving it open:
 * holding a TLS session's ~32 KB of mbedTLS buffers while talking plain HTTP would
 * waste exactly the heap this file works hardest to protect.
 */
WiFiClient& prepareTransport(const char* url) {
  const bool plain = std::strncmp(url, "http://", 7) == 0;
  WiFiClient* wanted =
      plain ? &s_plain_client : static_cast<WiFiClient*>(&s_tls_client);

  if (s_active_client != wanted) {
    dropConnection();
  }
  s_active_client = wanted;

  if (!wanted->connected()) {
    s_http.end();
    wanted->stop();
  }
  return *wanted;
}

bool fetchDirect(double center_lat, double center_lon, float fetch_radius_km);
bool fetchProxy(double center_lat, double center_lon, float fetch_radius_km);
void serviceSocialQueue();
void applySocialOutcomeLocally();

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

FeedSource lastFeedSource() { return s_last_source; }

bool proxyBackedOff() { return s_proxy_backed_off; }

void retryProxyNow() {
  if (!s_proxy_backed_off) {
    return;
  }
  s_proxy_backed_off = false;
  // Reset the accumulated penalty too: a user reaching for the tag button is a
  // stronger signal that the server is back than any timer we could pick.
  noteProxySuccess();
  Serial.println("adsb: proxy backoff cancelled on user request");
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  if (shouldUseProxy()) {
    if (fetchProxy(center_lat, center_lon, fetch_radius_km)) {
      noteProxySuccess();
      s_last_source = FeedSource::kProxy;
      return true;
    }
    noteProxyFailure();
    // Fall through to adsb.fi in this same cycle rather than showing an empty
    // radar for a poll: the aircraft matter more than the tags.
    dropConnection();
  }
  const bool ok = fetchDirect(center_lat, center_lon, fetch_radius_km);
  if (ok) {
    s_last_source = FeedSource::kDirect;
  }
  return ok;
}

namespace {

bool fetchDirect(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  // Fixed buffer URL instead of heap-allocating String fragments.
  char url[128];
  snprintf(url, sizeof(url), "%s%.6f/lon/%.6f/dist/%.1f",
           kApiBase, center_lat, center_lon, static_cast<double>(dist_nm));

  // Keep-alive: reuse the TLS session across polls so we skip the full
  // handshake every 3s (fast, smooth). The hazard on this RAM-tight board is a
  // re-handshake running while the previous session's ~32 KB of mbedTLS buffers
  // are still held: the fragmented heap starves it (-32512). So if the
  // connection has dropped, fully release it FIRST, then the fresh handshake
  // runs from a recovered heap. On any failure we drop it too, so the next poll
  // reconnects cleanly rather than re-handshaking in place.
  ensureHttpClient();
  WiFiClient& client = prepareTransport(url);

  if (!s_http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    dropConnection();
    return false;
  }

  s_http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(s_http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    s_http.end();
    s_tls_client.stop();
    return false;
  }

  WiFiClient* stream = s_http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no stream");
    s_http.end();
    s_tls_client.stop();
    return false;
  }
  const int content_length = s_http.getSize();
  const unsigned long deadline = millis() + kRequestTimeoutMs;

  // Parse straight from the TLS stream: no intermediate String payload buffer.
  initJsonFilter();
  JsonDocument doc;
  StreamJsonReader reader(stream, content_length, deadline);
  const DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(s_json_filter));
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    // Socket position is now uncertain; force a fresh connection next poll.
    s_http.end();
    s_tls_client.stop();
    return false;
  }
  // Drain trailing bytes so the reused keep-alive socket stays in sync.
  drainRemainder(stream, content_length, reader.bytesConsumed(), deadline);
  // Success: keep the connection open for the next poll (no end()/stop()).

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    s_aircraft[n].icao = 0;
    s_aircraft[n].is_military = false;
    if (plane["hex"].is<const char*>()) {
      const uint32_t hex_val =
          strtoul(plane["hex"].as<const char*>(), nullptr, 16);
      s_aircraft[n].icao = hex_val;
      s_aircraft[n].is_military = data::military::isMilitary(hex_val);
    }
    // The direct path is the fallback, so it carries no social tags. Clearing
    // these stops a stale handle from the last proxy response being drawn.
    s_aircraft[n].tag_handle[0] = '\0';
    s_aircraft[n].tag_is_mine = false;
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

bool fetchProxy(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  char url[192];
  snprintf(url, sizeof(url), "%s/v1/feed?lat=%.6f&lon=%.6f&dist=%.1f&gnd=%d",
           config::kFeedProxyBaseUrl, center_lat, center_lon,
           static_cast<double>(dist_nm),
           config::kAdsbShowGroundAircraft ? 1 : 0);

  ensureHttpClient();
  WiFiClient& client = prepareTransport(url);
  if (!s_http.begin(client, url)) {
    Serial.println("adsb: proxy http.begin failed");
    return false;
  }

  s_http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(s_http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: proxy HTTP %d\n", code);
    return false;
  }

  WiFiClient* stream = s_http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: proxy no stream");
    return false;
  }
  const int content_length = s_http.getSize();
  const unsigned long deadline = millis() + kRequestTimeoutMs;

  StreamLineReader reader(stream, content_length, deadline);
  char line[kFeedLineMax];
  FeedHeader header = {};
  FeedTag tags[kMaxFeedTags];
  size_t tag_count = 0;
  size_t n = 0;
  bool saw_header = false;

  while (reader.nextLine(line, sizeof(line))) {
    Aircraft parsed = {};
    FeedTag tag = {};
    switch (feedParseLine(line, &header, &parsed, &tag)) {
      case FeedLine::kHeader:
        saw_header = true;
        social::noteServerEpoch(header.server_epoch);
        break;
      case FeedLine::kAircraft:
        // The header is the first line, so anything claiming to be an aircraft
        // before it means this is not a PR1 body and must not reach the array.
        if (!saw_header) {
          break;
        }
        if (feedAircraftOnGround(parsed) && !config::kAdsbShowGroundAircraft) {
          break;
        }
        if (n < kMaxAircraft) {
          s_aircraft[n++] = parsed;
        }
        break;
      case FeedLine::kTag:
        if (tag_count < kMaxFeedTags) {
          tags[tag_count++] = tag;
        }
        break;
      case FeedLine::kInvalid:
        // One bad line does not condemn the payload; the header check below is
        // what decides whether this was a PR1 response at all.
        break;
    }
  }

  if (!saw_header) {
    Serial.println("adsb: proxy response was not PR1");
    // The array may hold half-written rows from a truncated body. Publishing zero
    // is honest; the direct fallback runs in this same cycle and refills it.
    s_aircraft_count = 0;
    return false;
  }

  // Tags are joined onto aircraft here rather than server-side so the response
  // stays identical for every device and therefore cacheable at the edge.
  if (ui::radar::socialEnabled()) {
    for (size_t t = 0; t < tag_count; ++t) {
      feedApplyTag(s_aircraft, n, tags[t], social::handle());
    }
  }

  drainRemainder(stream, content_length, reader.bytesConsumed(), deadline);
  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft, %u tags (proxy)\n",
                static_cast<unsigned>(n), static_cast<unsigned>(tag_count));

  // Same host, same keep-alive socket: piggybacking the claim here is what keeps
  // the device down to a single TLS session.
  serviceSocialQueue();
  return true;
}

/**
 * Send at most one queued claim/release per poll, on the connection the feed just
 * used. One per poll is deliberate: a tag is a single button press, so there is
 * never a backlog worth draining, and it bounds what a wedged queue can cost.
 */
void serviceSocialQueue() {
  social::Request request;
  if (!social::nextRequest(&request)) {
    return;
  }

  char url[192];
  snprintf(url, sizeof(url), "%s%s", config::kFeedProxyBaseUrl, request.path);
  // Same host as the feed, so this reuses the socket the GET just left open.
  WiFiClient& client = prepareTransport(url);
  if (!s_http.begin(client, url)) {
    social::completeRequest(0, nullptr);
    return;
  }

  s_http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  if (request.is_signed) {
    s_http.addHeader("X-Radar-Device", request.device);
    s_http.addHeader("X-Radar-Ts", request.timestamp);
    s_http.addHeader("X-Radar-Sig", request.signature);
  }

  const int code = s_http.POST(reinterpret_cast<uint8_t*>(request.body),
                               std::strlen(request.body));
  if (code <= 0) {
    Serial.printf("adsb: tag POST transport error %d\n", code);
    social::completeRequest(code, nullptr);
    dropConnection();
    return;
  }

  // Replies are a couple of short key=value lines, so a String is cheap here.
  const String body = s_http.getString();

  // Close the request out before returning. The feed GET path deliberately skips
  // end() to keep its keep-alive session, and GET-then-GET tolerates that, but
  // leaving a finished POST open poisons the next begin(): every subsequent
  // connection failed, on the proxy and on the adsb.fi fallback alike, until the
  // device was rebooted. setReuse(true) means end() resets the client state without
  // dropping the socket, so the next feed poll still reuses the connection.
  s_http.end();

  social::completeRequest(code, body.c_str());
  applySocialOutcomeLocally();
}

/**
 * Reflect a completed claim in the aircraft array straight away.
 *
 * The claim is sent *after* the feed for this cycle has already been parsed, so the
 * server's tag block does not include it until the next poll, and the poll after
 * that may still be serving a cached block. Waiting for the echo meant watching two
 * or three cycles pass before your own tag appeared. The next feed response
 * overwrites this either way, so a rejected claim corrects itself.
 */
void applySocialOutcomeLocally() {
  const social::PendingState state = social::pendingState();

  // Clearing everything carries no ICAO, so it is handled before the lookup.
  if (state == social::PendingState::kClearedAll) {
    for (size_t i = 0; i < s_aircraft_count; ++i) {
      if (s_aircraft[i].tag_is_mine) {
        s_aircraft[i].tag_handle[0] = '\0';
        s_aircraft[i].tag_is_mine = false;
      }
    }
    return;
  }

  const uint32_t icao = social::pendingIcao();
  if (icao == 0) {
    return;
  }

  switch (state) {
    case social::PendingState::kClaimed: {
      FeedTag tag = {};
      tag.icao = icao;
      std::strncpy(tag.handle, social::handle(), sizeof(tag.handle) - 1);
      feedApplyTag(s_aircraft, s_aircraft_count, tag, social::handle());
      break;
    }
    case social::PendingState::kReleased:
      for (size_t i = 0; i < s_aircraft_count; ++i) {
        if (s_aircraft[i].icao == icao) {
          s_aircraft[i].tag_handle[0] = '\0';
          s_aircraft[i].tag_is_mine = false;
        }
      }
      break;
    default:
      break;
  }
}

}  // namespace

namespace {

TaskHandle_t s_fetch_task_handle = nullptr;
volatile bool s_async_busy = false;
volatile bool s_async_result_ready = false;
volatile bool s_async_success = false;
double s_async_lat = 0;
double s_async_lon = 0;
float s_async_radius_km = 0;

void fetchTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    PollFn saved = s_poll_fn;
    s_poll_fn = nullptr;
    s_async_success = fetchUpdate(s_async_lat, s_async_lon, s_async_radius_km);
    s_poll_fn = saved;
    s_async_result_ready = true;
    s_async_busy = false;
  }
}

}  // namespace

bool fetchInit() {
  if (xTaskCreate(fetchTaskFn, "adsb", 8192, nullptr, 1,
                  &s_fetch_task_handle) != pdPASS) {
    // Leave the handle null so fetchStartAsync() refuses to latch s_async_busy;
    // otherwise the first request would block every later one forever.
    s_fetch_task_handle = nullptr;
    Serial.println("adsb: fetch task creation failed");
    return false;
  }
  return true;
}

void fetchStartAsync(double center_lat, double center_lon,
                     float fetch_radius_km) {
  if (s_async_busy || s_fetch_task_handle == nullptr) return;
  s_async_lat = center_lat;
  s_async_lon = center_lon;
  s_async_radius_km = fetch_radius_km;
  s_async_busy = true;
  s_async_result_ready = false;
  xTaskNotifyGive(s_fetch_task_handle);
}

bool fetchAsyncBusy() { return s_async_busy; }

bool fetchAsyncConsumeResult(bool* success) {
  if (!s_async_result_ready) return false;
  *success = s_async_success;
  s_async_result_ready = false;
  return true;
}

void resetConnection() {
  // The fetch task owns s_http/s_tls_client while running; don't touch them
  // concurrently. The caller is expected to gate on fetchAsyncBusy() too.
  if (s_async_busy) {
    return;
  }
  dropConnection();
  s_http_initialized = false;
}

}  // namespace services::adsb
