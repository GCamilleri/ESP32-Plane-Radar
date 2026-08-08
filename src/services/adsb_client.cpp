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
#include "services/adsb_parse.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

// Persistent TLS connection -- avoids a full handshake every poll cycle.
WiFiClientSecure s_tls_client;
HTTPClient s_http;
bool s_http_initialized = false;

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

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
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
  if (!s_http_initialized) {
    s_tls_client.setInsecure();
    s_http.setReuse(true);
    s_http_initialized = true;
  }

  if (!s_tls_client.connected()) {
    s_http.end();
    s_tls_client.stop();
  }

  if (!s_http.begin(s_tls_client, url)) {
    Serial.println("adsb: http.begin failed");
    s_http.end();
    s_tls_client.stop();
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
    s_aircraft[n].is_military = false;
    if (plane["hex"].is<const char*>()) {
      const uint32_t hex_val =
          strtoul(plane["hex"].as<const char*>(), nullptr, 16);
      s_aircraft[n].is_military = data::military::isMilitary(hex_val);
    }
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

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

void fetchInit() {
  xTaskCreate(fetchTaskFn, "adsb", 8192, nullptr, 1, &s_fetch_task_handle);
}

void fetchStartAsync(double center_lat, double center_lon,
                     float fetch_radius_km) {
  if (s_async_busy) return;
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
  s_http.end();
  s_tls_client.stop();
  s_http_initialized = false;
}

}  // namespace services::adsb
