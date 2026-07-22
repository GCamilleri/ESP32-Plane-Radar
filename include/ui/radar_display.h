#pragma once

#include <cstdint>

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** Set the number of consecutive ADS-B fetch failures (0 = last fetch succeeded). */
void radarDisplaySetFetchFailures(uint8_t failures);

}  // namespace ui
