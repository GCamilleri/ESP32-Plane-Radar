// Host tests for the PR1 proxy wire format. The fixtures below are real captured
// output from worker/src/index.ts, so a change to either side that breaks the
// other shows up here rather than on the bench.
#include <doctest.h>

#include <cstring>

#include "services/adsb_feed.h"

using services::adsb::Aircraft;
using services::adsb::FeedHeader;
using services::adsb::FeedLine;
using services::adsb::FeedTag;
using services::adsb::feedAircraftOnGround;
using services::adsb::feedApplyTag;
using services::adsb::feedParseLine;

namespace {

struct Parsed {
  FeedLine kind = FeedLine::kInvalid;
  FeedHeader header{};
  Aircraft aircraft{};
  FeedTag tag{};
};

/** feedParseLine mutates its input, so every case gets its own writable copy. */
Parsed parse(const char* text) {
  char buf[services::adsb::kFeedLineMax * 2];
  std::strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  Parsed out;
  out.kind = feedParseLine(buf, &out.header, &out.aircraft, &out.tag);
  return out;
}

}  // namespace

TEST_CASE("header carries epoch, lock and counts") {
  const Parsed p = parse("PR1 1787193390 1800 3 1");
  REQUIRE(p.kind == FeedLine::kHeader);
  CHECK(p.header.server_epoch == 1787193390u);
  CHECK(p.header.lock_sec == 1800);
  CHECK(p.header.aircraft_count == 3);
  CHECK(p.header.tag_count == 1);
}

TEST_CASE("aircraft line fills every field") {
  const Parsed p = parse("A,C82870,-41.43013,174.97026,210,212,234,ANZ783M,AT76,20025 ft");
  REQUIRE(p.kind == FeedLine::kAircraft);
  CHECK(p.aircraft.icao == 0xC82870u);
  CHECK(p.aircraft.lat == doctest::Approx(-41.43013f));
  CHECK(p.aircraft.lon == doctest::Approx(174.97026f));
  CHECK(p.aircraft.nose_deg == doctest::Approx(210.0f));
  CHECK(p.aircraft.track_deg == doctest::Approx(212.0f));
  CHECK(p.aircraft.gs_knots == doctest::Approx(234.0f));
  CHECK(std::strcmp(p.aircraft.callsign, "ANZ783M") == 0);
  CHECK(std::strcmp(p.aircraft.type, "AT76") == 0);
  CHECK(std::strcmp(p.aircraft.alt, "20025 ft") == 0);
  CHECK_FALSE(p.aircraft.is_military);
  CHECK(p.aircraft.tag_handle[0] == '\0');
  CHECK_FALSE(p.aircraft.tag_is_mine);
}

TEST_CASE("empty callsign falls back to the hex code") {
  const Parsed p = parse("A,C82870,-41.4,174.9,0,0,0,,,");
  REQUIRE(p.kind == FeedLine::kAircraft);
  CHECK(std::strcmp(p.aircraft.callsign, "C82870") == 0);
  CHECK(p.aircraft.type[0] == '\0');
  CHECK(p.aircraft.alt[0] == '\0');
}

TEST_CASE("military ranges are resolved on device, not sent by the proxy") {
  // 0x43C123 sits inside the UK military block in data/military_ranges.h.
  const Parsed p = parse("A,43C123,51.5,-0.1,90,90,300,RRR2233,A400,25000 ft");
  REQUIRE(p.kind == FeedLine::kAircraft);
  CHECK(p.aircraft.is_military);
}

TEST_CASE("oversized text fields are truncated, not overflowed") {
  const Parsed p =
      parse("A,C82870,-41.4,174.9,0,0,0,VERYLONGCALLSIGN,LONGTYPE,1234567890123456 ft");
  REQUIRE(p.kind == FeedLine::kAircraft);
  CHECK(std::strlen(p.aircraft.callsign) == sizeof(Aircraft::callsign) - 1);
  CHECK(std::strlen(p.aircraft.type) == sizeof(Aircraft::type) - 1);
  CHECK(std::strlen(p.aircraft.alt) == sizeof(Aircraft::alt) - 1);
}

TEST_CASE("ground aircraft are flagged by their altitude field") {
  const Parsed p = parse("A,C81102,-41.29,174.82,273,273,0,ZKTAW,PA38,GND");
  REQUIRE(p.kind == FeedLine::kAircraft);
  CHECK(feedAircraftOnGround(p.aircraft));
}

TEST_CASE("tag line carries icao, handle and remaining lock time") {
  const Parsed p = parse("T,4CA1FB,ZQN,1757");
  REQUIRE(p.kind == FeedLine::kTag);
  CHECK(p.tag.icao == 0x4CA1FBu);
  CHECK(std::strcmp(p.tag.handle, "ZQN") == 0);
  CHECK(p.tag.ttl_sec == 1757);
}

TEST_CASE("trailing CRLF and whitespace are tolerated") {
  const Parsed p = parse("T,4CA1FB,ZQN,1757\r\n");
  REQUIRE(p.kind == FeedLine::kTag);
  CHECK(p.tag.ttl_sec == 1757);
}

TEST_CASE("malformed lines are rejected rather than half-parsed") {
  CHECK(parse("").kind == FeedLine::kInvalid);
  CHECK(parse("X,C82870,1,2").kind == FeedLine::kInvalid);
  CHECK(parse("PR1 1787193390 1800").kind == FeedLine::kInvalid);
  // Truncated aircraft line: fewer fields than the format defines.
  CHECK(parse("A,C82870,-41.4,174.9,0,0,0").kind == FeedLine::kInvalid);
  // ICAO must be exactly six hex digits.
  CHECK(parse("A,C8287,-41.4,174.9,0,0,0,X,Y,Z").kind == FeedLine::kInvalid);
  CHECK(parse("A,C8287G,-41.4,174.9,0,0,0,X,Y,Z").kind == FeedLine::kInvalid);
  CHECK(parse("T,4CA1FB,,1757").kind == FeedLine::kInvalid);
  CHECK(parse("T,4CA1FB,ZQN").kind == FeedLine::kInvalid);
}

TEST_CASE("tags join onto aircraft by ICAO") {
  Aircraft planes[3] = {};
  planes[0].icao = 0x4CA1FBu;
  planes[1].icao = 0xC82870u;
  planes[2].icao = 0x4CA1FBu;  // same airframe seen twice should not happen, but be safe

  const FeedTag tag{0x4CA1FBu, {'Z', 'Q', 'N', '\0', '\0'}, 1757};

  CHECK(feedApplyTag(planes, 3, tag, "WLG") == 2);
  CHECK(std::strcmp(planes[0].tag_handle, "ZQN") == 0);
  CHECK_FALSE(planes[0].tag_is_mine);
  CHECK(planes[1].tag_handle[0] == '\0');
  CHECK(std::strcmp(planes[2].tag_handle, "ZQN") == 0);
}

TEST_CASE("a tag matching our own handle is marked as ours") {
  Aircraft plane{};
  plane.icao = 0x4CA1FBu;
  const FeedTag tag{0x4CA1FBu, {'Z', 'Q', 'N', '\0', '\0'}, 1757};

  CHECK(feedApplyTag(&plane, 1, tag, "ZQN") == 1);
  CHECK(plane.tag_is_mine);
}

TEST_CASE("an unregistered device owns no tags") {
  Aircraft plane{};
  plane.icao = 0x4CA1FBu;
  const FeedTag tag{0x4CA1FBu, {'Z', 'Q', 'N', '\0', '\0'}, 1757};

  CHECK(feedApplyTag(&plane, 1, tag, "") == 1);
  CHECK_FALSE(plane.tag_is_mine);
  CHECK(feedApplyTag(&plane, 1, tag, nullptr) == 1);
  CHECK_FALSE(plane.tag_is_mine);
}
