#pragma once

#include <cstdint>

namespace ui::target {

// Picking an aircraft on a radar with one button.
//
// The existing gestures are already spoken for: a tap cycles range and a 1 s hold
// opens the menu. So tagging lives behind a double tap, which was free. Inside the
// picker the same two gestures are reused with local meaning: tap moves the cursor
// to the next aircraft, hold claims or releases the tag.
//
// The picker times out rather than needing an explicit exit, because there is no
// button left to spend on one.

bool isOpen();

/** Enter the picker on the nearest aircraft. False when there is nothing to pick. */
bool open();
void close();

/** Consume button input and advance the state. Call each loop while open. */
void update();

/** ICAO of the highlighted aircraft, 0 when the picker is closed or empty. */
uint32_t selectedIcao();

/** Index into the current aircraft list, or -1. Valid only until the next fetch. */
int selectedIndex();

/**
 * Status text for the picker's footer: the callsign, or the outcome of the last
 * claim. Empty when the picker is closed.
 */
const char* statusText();

}  // namespace ui::target
