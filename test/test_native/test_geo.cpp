#include <doctest.h>

#include "ui/radar_geo.h"
#include "ui/radar_theme.h"

// Center/range/heading come from test/native/stubs.cpp:
// center = (52.3676, 4.9041), heading = 0, range outer = 10 km * 4/3.

using namespace ui;

TEST_CASE("distSqFromCenter") {
  CHECK(geo::distSqFromCenter(radar::kCenterX, radar::kCenterY) == 0);
  CHECK(geo::distSqFromCenter(radar::kCenterX + 3, radar::kCenterY + 4) == 25);
}

TEST_CASE("offsetKmFromCenter is zero at the center") {
  float dx = 0, dy = 0, dist = 0;
  geo::offsetKmFromCenter(52.3676f, 4.9041f, &dx, &dy, &dist);
  CHECK(dist == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("offsetKmFromCenter ~1 km due north") {
  float dx = 0, dy = 0, dist = 0;
  geo::offsetKmFromCenter(52.3676f + 1.0f / 111.0f, 4.9041f, &dx, &dy, &dist);
  CHECK(dy == doctest::Approx(1.0f).epsilon(0.02));
  CHECK(dx == doctest::Approx(0.0f).epsilon(0.001));
  CHECK(dist == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("latLonToScreen maps the center to the screen center") {
  int x = 0, y = 0;
  geo::latLonToScreen(52.3676f, 4.9041f, &x, &y);
  CHECK(x == radar::kCenterX);
  CHECK(y == radar::kCenterY);
}

TEST_CASE("latLonToScreen: north is up (smaller y), same column") {
  int x = 0, y = 0;
  geo::latLonToScreen(52.3676f + 1.0f / 111.0f, 4.9041f, &x, &y);
  CHECK(x == radar::kCenterX);   // pure north: no horizontal shift at heading 0
  CHECK(y < radar::kCenterY);    // screen y grows downward
}

TEST_CASE("clipPointToOuterRing keeps inside points, clips outside ones") {
  const int r = radar::kGridOuterRadius;

  int ix = radar::kCenterX + 3, iy = radar::kCenterY;
  geo::clipPointToOuterRing(radar::kCenterX, radar::kCenterY, &ix, &iy);
  CHECK(ix == radar::kCenterX + 3);  // already inside, unchanged
  CHECK(iy == radar::kCenterY);

  int ox = radar::kCenterX + 10 * r, oy = radar::kCenterY;
  geo::clipPointToOuterRing(radar::kCenterX, radar::kCenterY, &ox, &oy);
  CHECK(geo::distSqFromCenter(ox, oy) <= r * r);  // pulled onto/inside the ring
}
