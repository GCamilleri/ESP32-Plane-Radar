// Link-time stubs for the host geometry tests.
//
// radar_geo.cpp reads the radar center (services::location) and the current
// range/heading (ui::radar), which live in NVS-backed modules that do not build
// on the host. Provide fixed values so the projection math is deterministic:
// center = config default (Amsterdam), heading = 0 (north-up), range = 10 km.
#include "services/radar_location.h"
#include "ui/radar_range.h"

namespace services::location {
double lat() { return 52.3676; }
double lon() { return 4.9041; }
}  // namespace services::location

namespace ui::radar {
const RangePreset& rangeCurrent() {
  static const RangePreset preset{10.0f, 10.0f * kRing3ToOuterKm};
  return preset;
}
float headingDeg() { return 0.0f; }
}  // namespace ui::radar
