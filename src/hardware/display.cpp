#include "hardware/display.h"

#include "hardware/display_font.h"
#include "ui/radar_range.h"

LGFX tft;

void displayInit() {
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
}

void displayApplySavedBrightness() {
  tft.setBrightness(ui::radar::brightnessValue());
}
