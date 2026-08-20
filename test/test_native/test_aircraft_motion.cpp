#include <doctest.h>

#include <cstring>

#include "ui/aircraft_motion.h"
#include "ui/radar_geo.h"

using services::adsb::Aircraft;
using namespace ui;

namespace {

/** Northbound at 360 kt from a round starting point, which is 0.1852 km/s. */
Aircraft northbound(uint32_t icao, float lat = 52.0f, float lon = 5.0f) {
  Aircraft plane = {};
  plane.icao = icao;
  plane.lat = lat;
  plane.lon = lon;
  plane.nose_deg = 0.0f;
  plane.track_deg = 0.0f;
  plane.gs_knots = 360.0f;
  std::strcpy(plane.callsign, "TST123");
  return plane;
}

/** Degrees of latitude covered northbound at 360 kt in `sec`. */
float northDegrees(float sec) {
  return (360.0f * 1.852f / 3600.0f) * sec / geo::kKmPerDeg;
}

/** Run frames at 50 ms from `start_ms` for `sec`, as the render loop would. */
uint32_t runFrames(uint32_t start_ms, float sec) {
  const int frames = static_cast<int>(sec * 20.0f);
  uint32_t now = start_ms;
  for (int i = 0; i < frames; ++i) {
    now += 50;
    motion::advance(now);
  }
  return now;
}

}  // namespace

TEST_CASE("an untracked aircraft renders at its reported position") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  const motion::Motion state = motion::stateFor(plane);
  CHECK(state.lat == plane.lat);
  CHECK(state.lon == plane.lon);
  CHECK(state.track_deg == plane.track_deg);
}

TEST_CASE("an aircraft with no ICAO is never tracked") {
  motion::reset();
  const Aircraft plane = northbound(0);
  motion::onSnapshot(&plane, 1, 1000, 0);
  CHECK(motion::trackedCount() == 0);
  CHECK(motion::stateFor(plane).lat == plane.lat);
}

TEST_CASE("a new aircraft starts exactly where the feed put it") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 1000, 0);
  CHECK(motion::trackedCount() == 1);
  CHECK(motion::stateFor(plane).lat == doctest::Approx(plane.lat));
}

TEST_CASE("a new aircraft is placed forward of a stale block") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 10000, 3000);  // positions were 3 s old
  const float advanced = motion::stateFor(plane).lat - plane.lat;
  CHECK(advanced == doctest::Approx(northDegrees(3.0f)).epsilon(0.02));
}

TEST_CASE("position is dead reckoned along track between polls") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);  // establishes the frame clock, moves nothing

  const uint32_t now = runFrames(1000, 3.0f);
  CHECK(now == 4000);

  const motion::Motion state = motion::stateFor(plane);
  // Slightly behind the ideal 3 s of travel: the correction filter lags the
  // extrapolation by about one tau, which is what stops corrections snapping.
  const float ideal = northDegrees(3.0f);
  CHECK(state.lat - plane.lat > ideal * 0.8f);
  CHECK(state.lat - plane.lat < ideal);
  CHECK(state.lon == doctest::Approx(plane.lon));  // due north, no drift east
}

TEST_CASE("an unchanged fix keeps its baseline instead of restarting it") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);
  uint32_t now = runFrames(1000, 3.0f);
  const float before = motion::stateFor(plane).lat;

  // The same cached block comes back: identical coordinates, so nothing resets and
  // the aircraft keeps moving rather than being pulled back to the start.
  motion::onSnapshot(&plane, 1, now, 0);
  now = runFrames(now, 1.0f);
  CHECK(motion::stateFor(plane).lat > before);
  CHECK(motion::stateFor(plane).lat - plane.lat > northDegrees(3.2f));
  CHECK(motion::stateFor(plane).lat - plane.lat < northDegrees(4.0f));
}

TEST_CASE("a corrected fix is eased in, not snapped to") {
  motion::reset();
  Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);
  uint32_t now = runFrames(1000, 1.0f);
  const float before = motion::stateFor(plane).lat;

  // A fix half a degree north of anything the extrapolation predicted.
  plane.lat += 0.5f;
  motion::onSnapshot(&plane, 1, now, 0);

  motion::advance(now + 50);
  const float one_frame = motion::stateFor(plane).lat;
  CHECK(one_frame > before);              // moving towards it
  CHECK(one_frame < plane.lat - 0.3f);    // nowhere near arrived

  now = runFrames(now, 2.0f);
  CHECK(motion::stateFor(plane).lat == doctest::Approx(plane.lat).epsilon(0.02));
}

TEST_CASE("dead reckoning stops at the extrapolation cap") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);

  // Well past the cap, in frames so the filter has time to catch up.
  const uint32_t now = runFrames(1000, motion::kMaxExtrapolationSec + 20.0f);
  const float capped = northDegrees(motion::kMaxExtrapolationSec);
  const float travelled = motion::stateFor(plane).lat - plane.lat;
  CHECK(travelled == doctest::Approx(capped).epsilon(0.01));

  // And it stays put from there.
  runFrames(now, 5.0f);
  CHECK(motion::stateFor(plane).lat - plane.lat ==
        doctest::Approx(capped).epsilon(0.01));
}

TEST_CASE("track is smoothed the short way round the compass") {
  motion::reset();
  Aircraft plane = northbound(0xABC123);
  plane.track_deg = 350.0f;
  plane.nose_deg = 350.0f;
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);

  plane.track_deg = 10.0f;
  plane.nose_deg = 10.0f;
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1050);

  const float nose = motion::stateFor(plane).nose_deg;
  // Turning through north: either just under 360 or just over 0, never via 180.
  CHECK(((nose > 350.0f && nose <= 360.0f) || (nose >= 0.0f && nose < 10.0f)));
}

TEST_CASE("a heading reaches its target exactly, despite quantised storage") {
  // Attitude is stored in deci-degrees and knots to save RAM, so a filter step can
  // round to nothing. Without a snap, the drawn value stalls a fraction short of
  // the target and stays there for as long as the aircraft is on screen.
  motion::reset();
  Aircraft plane = northbound(0xABC123);
  plane.nose_deg = 90.0f;
  plane.track_deg = 90.0f;
  plane.gs_knots = 300.0f;
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);

  plane.nose_deg = 91.0f;
  plane.track_deg = 91.0f;
  plane.gs_knots = 301.0f;
  motion::onSnapshot(&plane, 1, 1000, 0);
  runFrames(1000, 3.0f);

  const motion::Motion state = motion::stateFor(plane);
  CHECK(state.nose_deg == doctest::Approx(91.0f).epsilon(0.002));
  CHECK(state.track_deg == doctest::Approx(91.0f).epsilon(0.002));
  CHECK(state.gs_knots == doctest::Approx(301.0f).epsilon(0.005));
}

TEST_CASE("label sides persist per aircraft and reset with the table") {
  motion::reset();
  const Aircraft first = northbound(0xABC123);
  const Aircraft second = northbound(0xDEF456);
  Aircraft both[2] = {first, second};
  motion::onSnapshot(both, 2, 1000, 0);

  CHECK(motion::labelSide(0xABC123) == motion::kNoLabelSide);
  motion::setLabelSide(0xABC123, 2);
  CHECK(motion::labelSide(0xABC123) == 2);
  CHECK(motion::labelSide(0xDEF456) == motion::kNoLabelSide);

  // An untracked aircraft has no side, and asking cannot create one.
  CHECK(motion::labelSide(0x999999) == motion::kNoLabelSide);
  motion::setLabelSide(0x999999, 1);
  CHECK(motion::labelSide(0x999999) == motion::kNoLabelSide);

  // A slot reused by a different aircraft starts with no side of its own.
  const Aircraft third = northbound(0x111111);
  motion::onSnapshot(&third, 1, 4000, 0);
  CHECK(motion::labelSide(0xABC123) == motion::kNoLabelSide);
  CHECK(motion::labelSide(0x111111) == motion::kNoLabelSide);
}

TEST_CASE("aircraft that leave the feed give their slots back") {
  motion::reset();

  Aircraft first[services::adsb::kMaxAircraft];
  for (size_t i = 0; i < services::adsb::kMaxAircraft; ++i) {
    first[i] = northbound(0x100000 + static_cast<uint32_t>(i));
  }
  motion::onSnapshot(first, services::adsb::kMaxAircraft, 1000, 0);
  CHECK(motion::trackedCount() == services::adsb::kMaxAircraft);

  // A completely different set of the same size: it only fits if every departed
  // aircraft was released first.
  Aircraft second[services::adsb::kMaxAircraft];
  for (size_t i = 0; i < services::adsb::kMaxAircraft; ++i) {
    second[i] = northbound(0x200000 + static_cast<uint32_t>(i));
  }
  motion::onSnapshot(second, services::adsb::kMaxAircraft, 4000, 0);
  CHECK(motion::trackedCount() == services::adsb::kMaxAircraft);
  for (const Aircraft& plane : second) {
    CHECK(motion::stateFor(plane).lat == doctest::Approx(plane.lat));
  }
}

TEST_CASE("a stationary aircraft does not drift") {
  motion::reset();
  Aircraft plane = northbound(0xABC123);
  plane.gs_knots = 0.0f;
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);
  runFrames(1000, 5.0f);
  CHECK(motion::stateFor(plane).lat == doctest::Approx(plane.lat));
  CHECK(motion::stateFor(plane).lon == doctest::Approx(plane.lon));
}

TEST_CASE("a long gap between frames cannot jump the filter") {
  motion::reset();
  const Aircraft plane = northbound(0xABC123);
  motion::onSnapshot(&plane, 1, 1000, 0);
  motion::advance(1000);

  // One frame five seconds later, as after a blocking reconnect: the correction is
  // limited to one capped step rather than the whole five seconds' worth.
  motion::advance(6000);
  const float travelled = motion::stateFor(plane).lat - plane.lat;
  const float ideal = northDegrees(5.0f);
  CHECK(travelled > 0.0f);
  CHECK(travelled < ideal * 0.75f);
}
