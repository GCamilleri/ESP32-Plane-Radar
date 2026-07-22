#pragma once

#include <cstdint>

namespace ui::menu {

void init();
bool isOpen();

/** Called every loop iteration.
 *  Consumes taps directly (instant, no debounce) and handles hold detection,
 *  timeouts, state transitions, and rendering. */
void update();

/** Open the menu from external trigger (e.g. double-tap in main loop). */
void open();

}  // namespace ui::menu
