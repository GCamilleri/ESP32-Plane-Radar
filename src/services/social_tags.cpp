#include "services/social_tags.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "ui/radar_range.h"

namespace services::social {

namespace {

constexpr char kPrefsNamespace[] = "social";
constexpr char kPrefsSecretKey[] = "secret";
constexpr char kPrefsHandleKey[] = "handle";

constexpr char kPathRegister[] = "/v1/register";
constexpr char kPathTag[] = "/v1/tag";
constexpr char kPathUntag[] = "/v1/untag";

/** Retry a failed registration no more often than this. */
constexpr unsigned long kRegisterRetryMs = 30000UL;

enum class Action : uint8_t { kNone, kRegister, kClaim, kRelease };

uint8_t s_secret[config::kSocialSecretBytes];
char s_secret_hex[config::kSocialSecretBytes * 2 + 1];
char s_device_id[kDeviceIdMax];
char s_handle[adsb::kTagHandleLen];
/** Handle the user asked for; may differ from s_handle until registration lands. */
char s_wanted_handle[adsb::kTagHandleLen];
bool s_registered = false;
bool s_initialised = false;
/** Separate flag rather than testing the timestamp for 0: millis() can be 0. */
bool s_register_attempted = false;
unsigned long s_last_register_attempt_ms = 0;

uint32_t s_server_epoch = 0;
unsigned long s_server_epoch_millis = 0;

// Shared between the loop task (queueing) and the fetch task (sending).
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Action s_queued_action = Action::kNone;
uint32_t s_queued_icao = 0;
Action s_in_flight_action = Action::kNone;
PendingState s_state = PendingState::kIdle;
uint32_t s_pending_icao = 0;
char s_pending_owner[adsb::kTagHandleLen];

void toHex(const uint8_t* bytes, size_t len, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kDigits[bytes[i] >> 4];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

const mbedtls_md_info_t* sha256Info() {
  return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
}

bool sha256(const uint8_t* input, size_t len, uint8_t out[32]) {
  const mbedtls_md_info_t* info = sha256Info();
  return info != nullptr && mbedtls_md(info, input, len, out) == 0;
}

bool hmacSha256Hex(const char* message, char* out_hex, size_t out_len) {
  if (out_len < 65) {
    return false;
  }
  const mbedtls_md_info_t* info = sha256Info();
  if (info == nullptr) {
    return false;
  }

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
            mbedtls_md_hmac_starts(&ctx, s_secret, sizeof(s_secret)) == 0 &&
            mbedtls_md_hmac_update(&ctx,
                                   reinterpret_cast<const uint8_t*>(message),
                                   std::strlen(message)) == 0;
  uint8_t mac[32];
  ok = ok && mbedtls_md_hmac_finish(&ctx, mac) == 0;
  mbedtls_md_free(&ctx);

  if (!ok) {
    return false;
  }
  toHex(mac, sizeof(mac), out_hex);
  return true;
}

/** 3-4 chars of [A-Z0-9]. Returns false when the input cannot be coerced. */
bool normaliseHandle(const char* raw, char* out, size_t out_len) {
  if (raw == nullptr || out_len < adsb::kTagHandleLen) {
    return false;
  }
  size_t n = 0;
  for (const char* p = raw; *p != '\0' && n < adsb::kTagHandleLen - 1; ++p) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out[n++] = c;
    }
  }
  out[n] = '\0';
  return n >= 3;
}

void loadOrCreateSecret() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    const size_t got =
        prefs.getBytes(kPrefsSecretKey, s_secret, sizeof(s_secret));
    if (got != sizeof(s_secret)) {
      esp_fill_random(s_secret, sizeof(s_secret));
      prefs.putBytes(kPrefsSecretKey, s_secret, sizeof(s_secret));
      Serial.println("social: generated new device secret");
    }
    char stored[adsb::kTagHandleLen] = {0};
    prefs.getString(kPrefsHandleKey, stored, sizeof(stored));
    if (!normaliseHandle(stored, s_wanted_handle, sizeof(s_wanted_handle))) {
      s_wanted_handle[0] = '\0';
    }
    prefs.end();
  } else {
    // No NVS: still usable this session, just not across reboots.
    esp_fill_random(s_secret, sizeof(s_secret));
    Serial.println("social: NVS unavailable, using a session-only identity");
  }
  toHex(s_secret, sizeof(s_secret), s_secret_hex);
}

void deriveDeviceId() {
  uint8_t digest[32] = {0};
  if (!sha256(s_secret, sizeof(s_secret), digest)) {
    // Falling back to the raw secret would leak it in every request header, so
    // disable the feature instead.
    s_device_id[0] = '\0';
    Serial.println("social: SHA-256 unavailable, tags disabled");
    return;
  }
  toHex(digest, 6, s_device_id);  // 12 hex chars, matches the Worker's regex
}

void setState(PendingState state, uint32_t icao, const char* owner) {
  portENTER_CRITICAL(&s_mux);
  s_state = state;
  s_pending_icao = icao;
  if (owner != nullptr) {
    std::strncpy(s_pending_owner, owner, sizeof(s_pending_owner) - 1);
    s_pending_owner[sizeof(s_pending_owner) - 1] = '\0';
  } else {
    s_pending_owner[0] = '\0';
  }
  portEXIT_CRITICAL(&s_mux);
}

/** Read the value of a `key=value` line out of a text/plain reply. */
bool findReplyValue(const char* body, const char* key, char* out,
                    size_t out_len) {
  if (body == nullptr || out_len == 0) {
    return false;
  }
  const size_t key_len = std::strlen(key);
  for (const char* p = body; *p != '\0';) {
    if (std::strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
      const char* value = p + key_len + 1;
      size_t n = 0;
      while (value[n] != '\0' && value[n] != '\n' && value[n] != '\r' &&
             n < out_len - 1) {
        out[n] = value[n];
        ++n;
      }
      out[n] = '\0';
      return n > 0;
    }
    const char* newline = std::strchr(p, '\n');
    if (newline == nullptr) {
      break;
    }
    p = newline + 1;
  }
  return false;
}

void queueAction(Action action, uint32_t icao) {
  if (!enabled()) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  s_queued_action = action;
  s_queued_icao = icao;
  s_state = PendingState::kQueued;
  s_pending_icao = icao;
  s_pending_owner[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace

void init() {
  if (s_initialised) {
    return;
  }
  s_handle[0] = '\0';
  s_wanted_handle[0] = '\0';
  s_pending_owner[0] = '\0';
  loadOrCreateSecret();
  deriveDeviceId();
  s_initialised = true;
  if (s_device_id[0] != '\0') {
    Serial.printf("social: device %s\n", s_device_id);
  }
}

bool enabled() {
  return s_initialised && s_device_id[0] != '\0' &&
         config::kFeedProxyBaseUrl[0] != '\0' && ui::radar::socialEnabled();
}

const char* handle() { return s_handle; }
const char* wantedHandle() {
  return s_handle[0] != '\0' ? s_handle : s_wanted_handle;
}
const char* deviceId() { return s_device_id; }
bool registered() { return s_registered; }

void saveHandleFromPortal(const char* value) {
  char normalised[adsb::kTagHandleLen] = {0};
  if (!normaliseHandle(value, normalised, sizeof(normalised))) {
    // Empty or unusable input means "no preference": the Worker derives one.
    return;
  }
  if (std::strcmp(normalised, s_wanted_handle) == 0) {
    return;
  }
  std::strncpy(s_wanted_handle, normalised, sizeof(s_wanted_handle) - 1);
  s_wanted_handle[sizeof(s_wanted_handle) - 1] = '\0';

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsHandleKey, s_wanted_handle);
    prefs.end();
  }
  // Re-register so the new handle takes effect without a reboot.
  s_registered = false;
  s_register_attempted = false;
  s_last_register_attempt_ms = 0;
  Serial.printf("social: handle set to %s\n", s_wanted_handle);
}

void noteServerEpoch(uint32_t epoch) {
  if (epoch == 0) {
    return;
  }
  s_server_epoch = epoch;
  s_server_epoch_millis = millis();
}

uint32_t epochNow() {
  if (s_server_epoch == 0) {
    return 0;
  }
  return s_server_epoch + (millis() - s_server_epoch_millis) / 1000UL;
}

void requestClaim(uint32_t icao) { queueAction(Action::kClaim, icao); }
void requestRelease(uint32_t icao) { queueAction(Action::kRelease, icao); }

PendingState pendingState() {
  portENTER_CRITICAL(&s_mux);
  const PendingState state = s_state;
  portEXIT_CRITICAL(&s_mux);
  return state;
}

uint32_t pendingIcao() {
  portENTER_CRITICAL(&s_mux);
  const uint32_t icao = s_pending_icao;
  portEXIT_CRITICAL(&s_mux);
  return icao;
}

const char* pendingOwner() { return s_pending_owner; }

void clearPending() {
  portENTER_CRITICAL(&s_mux);
  if (s_state != PendingState::kQueued && s_state != PendingState::kInFlight) {
    s_state = PendingState::kIdle;
    s_pending_icao = 0;
    s_pending_owner[0] = '\0';
  }
  portEXIT_CRITICAL(&s_mux);
}

bool nextRequest(Request* out) {
  if (out == nullptr || !enabled()) {
    return false;
  }

  portENTER_CRITICAL(&s_mux);
  const Action queued = s_queued_action;
  const uint32_t icao = s_queued_icao;
  portEXIT_CRITICAL(&s_mux);

  // Registration comes first: a claim from an unregistered device is rejected,
  // and the reply is what tells us our handle.
  Action action = Action::kNone;
  if (!s_registered) {
    if (!s_register_attempted ||
        millis() - s_last_register_attempt_ms >= kRegisterRetryMs) {
      action = Action::kRegister;
    } else if (queued != Action::kNone) {
      return false;  // hold the claim until registration succeeds
    }
  } else if (queued != Action::kNone) {
    action = queued;
  }

  if (action == Action::kNone) {
    return false;
  }

  std::memset(out, 0, sizeof(*out));
  std::strncpy(out->device, s_device_id, sizeof(out->device) - 1);

  if (action == Action::kRegister) {
    s_register_attempted = true;
    s_last_register_attempt_ms = millis();
    std::strncpy(out->path, kPathRegister, sizeof(out->path) - 1);
    snprintf(out->body, sizeof(out->body), "dev=%s&secret=%s&handle=%s",
             s_device_id, s_secret_hex, s_wanted_handle);
    out->is_signed = false;
    s_in_flight_action = Action::kRegister;
    return true;
  }

  const uint32_t now = epochNow();
  if (now == 0) {
    // No feed response yet, so no clock. The Worker would reject the signature.
    return false;
  }

  std::strncpy(out->path,
               action == Action::kClaim ? kPathTag : kPathUntag,
               sizeof(out->path) - 1);
  snprintf(out->body, sizeof(out->body), "icao=%06lX",
           static_cast<unsigned long>(icao & 0xFFFFFFu));
  snprintf(out->timestamp, sizeof(out->timestamp), "%lu",
           static_cast<unsigned long>(now));

  char message[kRequestPathMax + kRequestBodyMax + 32];
  snprintf(message, sizeof(message), "POST\n%s\n%s\n%s", out->path,
           out->timestamp, out->body);
  if (!hmacSha256Hex(message, out->signature, sizeof(out->signature))) {
    setState(PendingState::kError, icao, nullptr);
    portENTER_CRITICAL(&s_mux);
    s_queued_action = Action::kNone;
    portEXIT_CRITICAL(&s_mux);
    return false;
  }
  out->is_signed = true;

  portENTER_CRITICAL(&s_mux);
  s_queued_action = Action::kNone;  // taken; a retry is the user's call
  s_in_flight_action = action;
  s_state = PendingState::kInFlight;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void completeRequest(int http_code, const char* body) {
  const Action action = s_in_flight_action;
  s_in_flight_action = Action::kNone;
  const uint32_t icao = pendingIcao();

  if (action == Action::kRegister) {
    char assigned[adsb::kTagHandleLen] = {0};
    if (http_code == 200 &&
        findReplyValue(body, "handle", assigned, sizeof(assigned))) {
      std::strncpy(s_handle, assigned, sizeof(s_handle) - 1);
      s_handle[sizeof(s_handle) - 1] = '\0';
      s_registered = true;
      Serial.printf("social: registered as %s\n", s_handle);
    } else {
      Serial.printf("social: registration failed (HTTP %d)\n", http_code);
    }
    return;
  }

  if (action == Action::kNone) {
    return;
  }

  switch (http_code) {
    case 200:
      setState(action == Action::kClaim ? PendingState::kClaimed
                                       : PendingState::kReleased,
               icao, nullptr);
      break;
    case 409: {
      char owner[adsb::kTagHandleLen] = {0};
      findReplyValue(body, "handle", owner, sizeof(owner));
      setState(PendingState::kDenied, icao, owner);
      break;
    }
    case 401:
      // Our identity is not what the Worker thinks it is; re-register at once.
      s_registered = false;
      s_register_attempted = false;
      s_last_register_attempt_ms = 0;
      setState(PendingState::kError, icao, nullptr);
      break;
    default:
      Serial.printf("social: tag request failed (HTTP %d)\n", http_code);
      setState(PendingState::kError, icao, nullptr);
      break;
  }
}

}  // namespace services::social
