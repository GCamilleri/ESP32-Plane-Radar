#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "ui/radar_geo.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace ui {
namespace radar {

uint16_t gColorBackground = 0x0000;
uint16_t gColorGrid = 0x0320;
uint16_t gColorLabel = 0xFFFF;
uint16_t gColorCenter = 0xFFFF;
uint16_t gColorAircraft = 0x001F;
uint16_t gColorTrackVector = 0xFFFF;
uint16_t gColorTagType = 0x5DFF;
uint16_t gColorTagAltitude = 0xFFE0;
uint16_t gColorRunway = 0x4D5F;
uint16_t gColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

uint8_t s_fetch_failures = 0;

float s_sweep_angle_deg = 0.0f;
unsigned long s_last_sweep_ms = 0;

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &lgfx::v1::fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &lgfx::v1::fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &lgfx::v1::fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = displayFontFindVlwSizeForHeight(tft, cardinal_target);
    const int cardinal_h = displayFontMeasureVlwHeight(tft, s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = displayFontFindVlwSizeForHeight(tft, scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&lgfx::v1::fonts::FreeSansBold12pt7b,
                                                  &lgfx::v1::fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&lgfx::v1::fonts::FreeSansBold9pt7b,
                                               &lgfx::v1::fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = displayFontFindVlwSizeForHeight(tft, target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&lgfx::v1::fonts::FreeSansBold12pt7b,
                                               &lgfx::v1::fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  radar::gColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::gColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::gColorLabel = tft.color565(255, 255, 255);
  radar::gColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::gColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::gColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::gColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::gColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::gColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::gColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::gColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return geo::distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  geo::offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float geo_angle_rad = atan2f(dx_km, dy_km);
  const float heading_rad = radar::headingDeg() * 0.01745329252f;
  const float screen_angle = geo_angle_rad - heading_rad;

  *out_x = cx + static_cast<int>(lroundf(sinf(screen_angle) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(screen_angle) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::gColorAircraft);
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  geo::clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    const int w = s_draw->textWidth(plane.callsign);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane) {
  initTagLabelMetrics();
  applyTagStyle();

  const uint8_t lbl_mode = radar::labelMode();
  const int line_count = (lbl_mode == 0) ? 3 : 1;
  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * line_count;
  int ly = y - block_h / 2;

  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(textdatum_t::top_right);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::gColorLabel, radar::gColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  if (lbl_mode != 0) return;
  ly += line_h;

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::gColorTagType, radar::gColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(radar::gColorTagAltitude, radar::gColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

// Scratch arrays for per-frame draw sorting.  File-scope static keeps ~1.8 KB
// off the 8 KB Arduino loop stack where deep LovyanGFX draw recursion lives.
static AircraftDrawItem s_draw_items[services::adsb::kMaxAircraft];
static BeyondDotDrawItem s_draw_dots[services::adsb::kMaxAircraft];

struct LabelPlacement {
  int16_t x, y, w, h;   // bounding box of placed label
  bool on_right;         // true = left-aligned text, false = right-aligned
  uint8_t src_draw_idx;  // index into s_draw_items
};
static LabelPlacement s_label_placements[services::adsb::kMaxAircraft];
static size_t s_label_count = 0;

int overlapArea(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                int16_t bx, int16_t by, int16_t bw, int16_t bh) {
  const int ox = std::max(0, std::min(ax + aw, bx + bw) - std::max(ax, bx));
  const int oy = std::max(0, std::min(ay + ah, by + bh) - std::max(ay, by));
  return ox * oy;
}

void resolveLabels(size_t draw_count) {
  s_label_count = 0;
  const uint8_t lbl_mode = radar::labelMode();
  if (lbl_mode >= 2) return;

  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int line_count = (lbl_mode == 0) ? 3 : 1;
  const int block_h = line_h * line_count;
  const int symbol_half = radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  const int gap = radar::kAircraftLabelGapPx;

  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  // Process closest-to-center first (end of s_draw_items, which is sorted
  // farthest-first).  Closer aircraft get label-position priority.
  for (int d = static_cast<int>(draw_count) - 1; d >= 0; --d) {
    const size_t i = s_draw_items[d].index;
    const int ax = s_draw_items[d].x;
    const int ay = s_draw_items[d].y;

    const int block_w = measureTagBlockWidth(planes[i]);
    if (block_w <= 0) continue;

    // 4 candidate positions: right, left, above, below
    struct Candidate {
      int16_t x, y;
      bool on_right;
    };

    const Candidate candidates[4] = {
        // Right of symbol
        {static_cast<int16_t>(ax + symbol_half + gap),
         static_cast<int16_t>(ay - block_h / 2), true},
        // Left of symbol
        {static_cast<int16_t>(ax - symbol_half - gap - block_w),
         static_cast<int16_t>(ay - block_h / 2), false},
        // Above symbol
        {static_cast<int16_t>(ax - block_w / 2),
         static_cast<int16_t>(ay - symbol_half - gap - block_h), true},
        // Below symbol
        {static_cast<int16_t>(ax - block_w / 2),
         static_cast<int16_t>(ay + symbol_half + gap), true},
    };

    int best_candidate = 0;
    int best_overlap = INT32_MAX;

    for (int c = 0; c < 4; ++c) {
      int total_overlap = 0;
      for (size_t p = 0; p < s_label_count; ++p) {
        total_overlap += overlapArea(
            candidates[c].x, candidates[c].y,
            static_cast<int16_t>(block_w), static_cast<int16_t>(block_h),
            s_label_placements[p].x, s_label_placements[p].y,
            s_label_placements[p].w, s_label_placements[p].h);
      }
      if (total_overlap == 0) {
        best_candidate = c;
        break;
      }
      if (total_overlap < best_overlap) {
        best_overlap = total_overlap;
        best_candidate = c;
      }
    }

    auto& placement = s_label_placements[s_label_count];
    placement.x = candidates[best_candidate].x;
    placement.y = candidates[best_candidate].y;
    placement.w = static_cast<int16_t>(block_w);
    placement.h = static_cast<int16_t>(block_h);
    placement.on_right = candidates[best_candidate].on_right;
    placement.src_draw_idx = static_cast<uint8_t>(d);

    // Clamp to screen bounds
    placement.x = static_cast<int16_t>(
        std::max(1, std::min(static_cast<int>(placement.x),
                             radar::kSize - block_w - 1)));
    placement.y = static_cast<int16_t>(
        std::max(1, std::min(static_cast<int>(placement.y),
                             radar::kSize - block_h - 1)));

    ++s_label_count;
  }
}

void drawAircraftTagPlaced(const LabelPlacement& place,
                           const services::adsb::Aircraft& plane) {
  initTagLabelMetrics();
  applyTagStyle();

  const uint8_t lbl_mode = radar::labelMode();
  const int line_h = s_draw->fontHeight();
  int ly = place.y;

  int anchor_x;
  if (place.on_right) {
    anchor_x = place.x;
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = place.x + place.w;
    s_draw->setTextDatum(textdatum_t::top_right);
  }

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::gColorLabel, radar::gColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  if (lbl_mode != 0) return;
  ly += line_h;

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::gColorTagType, radar::gColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(radar::gColorTagAltitude, radar::gColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

void drawSweep() {
  if (!radar::sweepEnabled()) return;

  const unsigned long now = millis();
  if (s_last_sweep_ms == 0) {
    s_last_sweep_ms = now;
    return;
  }

  const float elapsed_s = static_cast<float>(now - s_last_sweep_ms) / 1000.0f;
  s_last_sweep_ms = now;

  s_sweep_angle_deg += radar::kSweepDegreesPerSec * elapsed_s;
  if (s_sweep_angle_deg >= 360.0f) s_sweep_angle_deg -= 360.0f;

  constexpr float kDegToRad = 0.01745329252f;
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;

  for (int t = radar::kSweepTrailCount; t >= 0; --t) {
    const float angle =
        (s_sweep_angle_deg - t * radar::kSweepTrailSpacingDeg) * kDegToRad;
    const int ex = cx + static_cast<int>(lroundf(sinf(angle) * r));
    const int ey = cy - static_cast<int>(lroundf(cosf(angle) * r));

    uint8_t green;
    if (t == 0) {
      green = radar::kSweepLeadingGreen;
    } else {
      green = static_cast<uint8_t>(100 / t);
    }

    const uint16_t color = s_draw->color565(0, green, 0);
    s_draw->drawLine(cx, cy, ex, ey, color);
  }
}

void drawAircraft() {
  initLabelMetrics();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    geo::offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      geo::latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      s_draw_items[draw_count].index = i;
      s_draw_items[draw_count].x = x;
      s_draw_items[draw_count].y = y;
      s_draw_items[draw_count].dist_sq = geo::distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    s_draw_dots[dot_count].x = dot_x;
    s_draw_dots[dot_count].y = dot_y;
    s_draw_dots[dot_count].dist_sq = geo::distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  std::sort(s_draw_dots, s_draw_dots + dot_count,
            [](const BeyondDotDrawItem& a, const BeyondDotDrawItem& b) {
              return a.dist_sq > b.dist_sq;
            });
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(s_draw_dots[d].x, s_draw_dots[d].y);
  }

  std::sort(s_draw_items, s_draw_items + draw_count,
            [](const AircraftDrawItem& a, const AircraftDrawItem& b) {
              return a.dist_sq > b.dist_sq;
            });
  const float h_deg = radar::headingDeg();
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = s_draw_items[d].index;
    const int x = s_draw_items[d].x;
    const int y = s_draw_items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg - h_deg,
                    planes[i].track_deg - h_deg,
                    planes[i].gs_knots, radar::gColorTrackVector);
    drawHeadingTriangle(x, y, planes[i].nose_deg - h_deg,
                        radar::gColorAircraft);
  }
  const uint8_t lbl_mode = radar::labelMode();
  if (lbl_mode < 2) {
    resolveLabels(draw_count);
    for (size_t p = 0; p < s_label_count; ++p) {
      const size_t d = s_label_placements[p].src_draw_idx;
      const size_t i = s_draw_items[d].index;
      drawAircraftTagPlaced(s_label_placements[p], planes[i]);
    }
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::gColorLabel, radar::gColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::gColorBackground);
  s_draw->setTextColor(radar::gColorGrid, radar::gColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::gColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  uint8_t r, g, b;
  if (s_fetch_failures < 3) {
    r = radar::kFreshR; g = radar::kFreshG; b = radar::kFreshB;
  } else if (s_fetch_failures < 10) {
    r = radar::kAgingR; g = radar::kAgingG; b = radar::kAgingB;
  } else {
    r = radar::kStaleR; g = radar::kStaleG; b = radar::kStaleB;
  }
  uint16_t color;
  if (config::kDisplayRgbOrder) {
    color = tft.color565(b, g, r);
  } else {
    color = tft.color565(r, g, b);
  }
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, color);
}

void drawCardinalLabels() {
  constexpr float kDegToRad = 0.01745329252f;
  constexpr const char* kCardinals[] = {"N", "E", "S", "W"};
  constexpr float kBearings[] = {0.0f, 90.0f, 180.0f, 270.0f};
  const float heading = radar::headingDeg();
  const int r = radar::kCenterX - 2;

  for (int i = 0; i < 4; ++i) {
    const float rad = (kBearings[i] - heading) * kDegToRad;
    const float sin_a = sinf(rad);
    const float cos_a = cosf(rad);
    const int x = radar::kCenterX + static_cast<int>(lroundf(sin_a * r));
    const int y = radar::kCenterY - static_cast<int>(lroundf(cos_a * r));

    textdatum_t datum;
    if (fabsf(sin_a) > fabsf(cos_a)) {
      datum = sin_a > 0 ? textdatum_t::middle_right : textdatum_t::middle_left;
    } else {
      datum = cos_a > 0 ? textdatum_t::top_center : textdatum_t::bottom_center;
    }
    drawCardinalLabel(kCardinals[i], x, y, datum);
  }
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::gColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::gColorGrid);
  initPalette();
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(16);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawSweep();
    drawAircraft();
  }
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  // initPalette() is called inside drawStaticGrid(), no need to repeat here.
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawSweep();
  drawAircraft();
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  // initPalette() is called inside drawStaticGrid(), no need to repeat here.
  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

void radarDisplaySetFetchFailures(uint8_t failures) {
  s_fetch_failures = failures;
}

}  // namespace ui
