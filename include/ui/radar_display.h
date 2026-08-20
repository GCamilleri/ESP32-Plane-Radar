#pragma once

#include <cstdint>

namespace ui {

/**
 * Allocate the off-screen frame buffer, before anything else has claimed heap.
 *
 * Optional: the first draw allocates it anyway. Calling it early is about *where*
 * the 115 KB lands. Taken after the radio is up, it is carved out of a heap already
 * populated with WiFi and LwIP buffers, and what remains is fragments: on a C3 the
 * largest free block was measured at 32756 bytes, which is too tight for mbedTLS's
 * two 16 KB content buffers, so TLS handshakes failed with -32512 and the radar lost
 * every data source. Taken first, the big block sits at one end and the rest of the
 * heap stays contiguous.
 */
void radarDisplayReserveFrame();

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** Set the number of consecutive ADS-B fetch failures (0 = last fetch succeeded). */
void radarDisplaySetFetchFailures(uint8_t failures);

}  // namespace ui
