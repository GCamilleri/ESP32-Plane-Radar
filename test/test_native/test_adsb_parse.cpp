#include <doctest.h>

#include <ArduinoJson.h>

#include <string>

#include "services/adsb_parse.h"

using namespace services::adsb;

static JsonObject parseOne(JsonDocument& doc, const char* json) {
  const DeserializationError err = deserializeJson(doc, json);
  REQUIRE(err == DeserializationError::Ok);
  return doc.as<JsonObject>();
}

TEST_CASE("pickNoseHeading fallback: true_heading > mag_heading > track > dir") {
  JsonDocument d1;
  CHECK(pickNoseHeading(parseOne(
            d1, R"({"true_heading":10,"mag_heading":20,"track":30,"dir":40})")) ==
        doctest::Approx(10));
  JsonDocument d2;
  CHECK(pickNoseHeading(parseOne(d2, R"({"mag_heading":20,"track":30})")) ==
        doctest::Approx(20));
  JsonDocument d3;
  CHECK(pickNoseHeading(parseOne(d3, R"({"track":30})")) == doctest::Approx(30));
  JsonDocument d4;
  CHECK(pickNoseHeading(parseOne(d4, R"({"dir":40})")) == doctest::Approx(40));
  JsonDocument d5;
  CHECK(pickNoseHeading(parseOne(d5, R"({})")) == doctest::Approx(0));
}

TEST_CASE("pickTrackHeading prefers track over true_heading") {
  JsonDocument d;
  CHECK(pickTrackHeading(parseOne(d, R"({"true_heading":10,"track":30})")) ==
        doctest::Approx(30));
}

TEST_CASE("pickGroundSpeed fallback: gs > tas > ias") {
  JsonDocument d1;
  CHECK(pickGroundSpeed(parseOne(d1, R"({"gs":100,"tas":200})")) ==
        doctest::Approx(100));
  JsonDocument d2;
  CHECK(pickGroundSpeed(parseOne(d2, R"({"tas":200,"ias":150})")) ==
        doctest::Approx(200));
  JsonDocument d3;
  CHECK(pickGroundSpeed(parseOne(d3, R"({"ias":150})")) == doctest::Approx(150));
}

TEST_CASE("isOnGround only for the literal string \"ground\"") {
  JsonDocument d1;
  CHECK(isOnGround(parseOne(d1, R"({"alt_baro":"ground"})")));
  JsonDocument d2;
  CHECK_FALSE(isOnGround(parseOne(d2, R"({"alt_baro":3000})")));
  JsonDocument d3;
  CHECK_FALSE(isOnGround(parseOne(d3, R"({})")));
}

TEST_CASE("fillTagFields: callsign trimmed, type, numeric altitude") {
  Aircraft ac{};
  JsonDocument d;
  fillTagFields(&ac, parseOne(
                         d, R"({"flight":"KLM123  ","t":"B738","alt_baro":3000})"));
  CHECK(std::string(ac.callsign) == "KLM123");  // trailing spaces trimmed
  CHECK(std::string(ac.type) == "B738");
  CHECK(std::string(ac.alt) == "3000 ft");
}

TEST_CASE("fillTagFields: falls back to hex, formats ground altitude") {
  Aircraft ac{};
  JsonDocument d;
  fillTagFields(&ac, parseOne(d, R"({"hex":"4CA123","alt_baro":"ground"})"));
  CHECK(std::string(ac.callsign) == "4CA123");  // no flight -> hex
  CHECK(std::string(ac.alt) == "GND");
}
