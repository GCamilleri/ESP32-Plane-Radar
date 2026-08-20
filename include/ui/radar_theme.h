#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

constexpr int kSize = 240;
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;

/** Outermost grid ring (inside edge labels). */
constexpr int kGridOuterRadius = 107;

/** N: offset from top edge (top_center, negative = up). */
constexpr int kCardinalNorthOffsetY = -1;
/** S: offset from bottom edge (bottom_center, positive = down). */
constexpr int kCardinalSouthOffsetY = 3;

/** Gap between scale label right edge and outer ring on the east spoke (px). */
constexpr int kScaleGapFromOuterRing = 6;

/** Target cap height (px) for N/S/E/W. */
constexpr int kCardinalLabelHeightPx = 14;
/** Scale label is this many px shorter than cardinals. */
constexpr int kScaleBelowCardinalPx = 3;

constexpr int kRingCount = 4;

/** Shared grid stroke: drawWideLine half-width (~2 px total); rings use the same px count. */
constexpr float kGridStrokeHalfWidth = 1.0f;

constexpr int kCenterDotRadius = 2;

/** Filled aircraft symbol (nose triangle). */
constexpr int kAircraftNoseLenPx = 8;
constexpr int kAircraftTailLenPx = 3;
constexpr int kAircraftTailHalfPx = 4;
/** Track vector: ground distance covered in this many seconds at current gs. */
constexpr float kAircraftTrackHorizonSec = 60.0f;
/** Minimum visible vector when gs > 0 (px). */
constexpr int kAircraftSpeedLineMinPx = 2;
/** Track line length uses this outer_km, not the active range preset. */
constexpr float kAircraftTrackRefOuterKm = 13.3f;
/** Shorter than full 60 s horizon at ref scale; ×1.5 length boost applied. */
constexpr float kAircraftTrackLengthScale = 1.5f / 5.0f;
/** drawWideLine half-width for speed vectors (~2 px total). */
constexpr float kAircraftTrackLineHalfWidth = 1.0f;

constexpr float kRunwayLineWidthPx = 2.0f;
constexpr float kRunwayLineHalfWidth = kRunwayLineWidthPx * 0.5f;
constexpr int kRunwayLabelHeightPx = kCardinalLabelHeightPx;
constexpr int kRunwayLabelGapPx = 12;
/** Gap from triangle edge to tag block (px). */
constexpr int kAircraftLabelGapPx = 1;
/** Keep symbol centroid inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx =
    kAircraftNoseLenPx + kAircraftTailHalfPx + 1;

/** Beyond-ring traffic: bearing cues on screen rim (correct direction, fixed radius). */
constexpr int kBeyondRingDotRadiusPx = 4;
constexpr int kBeyondRingScreenMarginPx = 2;
/** Target cap height (px) for aircraft tags (bold, slightly above scale label). */
constexpr int kAircraftTagLabelHeightPx = 13;

/** RGB565 palette targets (applied in initPalette). */
constexpr uint8_t kBgR = 4;
constexpr uint8_t kBgG = 10;
constexpr uint8_t kBgB = 28;
constexpr uint8_t kGridR = 16;
constexpr uint8_t kGridG = 100;
constexpr uint8_t kGridB = 32;
constexpr uint8_t kAircraftR = 255;
constexpr uint8_t kAircraftG = 0;
constexpr uint8_t kAircraftB = 0;
constexpr uint8_t kMilitaryR = 0;
constexpr uint8_t kMilitaryG = 230;
constexpr uint8_t kMilitaryB = 255;
constexpr uint8_t kTrackR = 255;
constexpr uint8_t kTrackG = 0;
constexpr uint8_t kTrackB = 255;
constexpr uint8_t kTagTypeR = 255;
constexpr uint8_t kTagTypeG = 200;
constexpr uint8_t kTagTypeB = 0;
constexpr uint8_t kTagAltR = 90;
constexpr uint8_t kTagAltG = 200;
constexpr uint8_t kTagAltB = 255;
constexpr uint8_t kRunwayR = 90;
constexpr uint8_t kRunwayG = 70;
constexpr uint8_t kRunwayB = 160;
constexpr uint8_t kRunwayLabelR = 140;
constexpr uint8_t kRunwayLabelG = 120;
constexpr uint8_t kRunwayLabelB = 200;

/**
 * Social tags. A tagged aircraft keeps its normal symbol colour and gains a corner
 * bracket reticle plus the tagger's handle, so the tag reads as an annotation
 * rather than as a different kind of contact.
 *
 * The reticle colour comes from the handle, which is why it is a fixed palette
 * rather than a hash straight to RGB, and the same tagger renders identically on
 * every radar.
 *
 * Chosen by scripts/pick_tag_palette.py rather than by eye. The radar already
 * spends most of the colour wheel on meaning (red aircraft, cyan military, amber
 * type, light-blue altitude, magenta track vectors, violet runways, white labels),
 * and an eyeballed palette put two tags right next to the altitude colour. The
 * script maximises the worst-case CIELAB distance to every reserved colour, after
 * quantising to RGB565 because that is what the panel shows. Current worst case is
 * deltaE 40, where 2.3 is "just noticeable".
 *
 * Five entries, not more: handles collide sooner, but two taggers sharing a colour
 * is far less confusing than a tag being mistaken for an altitude readout. Re-run
 * the script if any of the reserved colours below ever change.
 */
constexpr uint8_t kTagPalette[][3] = {
    {255, 44, 140},   // hot pink
    {99, 255, 181},   // mint
    {222, 117, 82},   // salmon
    {214, 255, 41},   // chartreuse
    {165, 97, 255},   // purple
};
constexpr size_t kTagPaletteCount = sizeof(kTagPalette) / sizeof(kTagPalette[0]);

/**
 * Palette slot for a handle. FNV-1a, so it is stable across builds and identical
 * on every device: two radars must draw the same tagger in the same colour, or the
 * colour stops carrying any meaning.
 */
inline size_t tagPaletteIndex(const char* handle) {
  uint32_t hash = 0x811c9dc5u;
  for (const char* p = handle; p != nullptr && *p != '\0'; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 0x01000193u;
  }
  return hash % kTagPaletteCount;
}

/**
 * Drawn in front of a handle so a tag cannot be read as a callsign, type code or
 * airport ident, which is what a bare 3-4 character code looks like on a radar.
 *
 * Not a hyphen: ZK-ABC is the aircraft registration format, so a hyphenated code
 * would read as a tail number, which is worse than no separator at all. '@' appears
 * nowhere in aviation identifiers and is present in the embedded VLW font.
 *
 * Display only. The handle on the wire stays [A-Z0-9] so the palette hash and the
 * server's validation are unaffected.
 */
constexpr char kTagHandleSigil[] = "@";
/** "@" + 4 handle chars + terminator. */
constexpr size_t kTagHandleLabelMax = 8;

/** The picker's cursor: white, so it never looks like anyone's tag colour. */
constexpr uint8_t kSelectR = 255, kSelectG = 255, kSelectB = 255;

/** Corner bracket reticle: distance from the symbol centre to the bracket. */
constexpr int kTargetBracketRadiusPx = kAircraftNoseLenPx + 3;
/** Length of each arm of a bracket. */
constexpr int kTargetBracketArmPx = 4;
/** The picker's cursor sits just outside a tag reticle so both can be seen at once. */
constexpr int kSelectBracketRadiusPx = kTargetBracketRadiusPx + 4;

/** Data freshness indicator dot. */
constexpr uint8_t kFreshR = 0, kFreshG = 200, kFreshB = 0;
constexpr uint8_t kAgingR = 255, kAgingG = 180, kAgingB = 0;
constexpr uint8_t kStaleR = 255, kStaleG = 0, kStaleB = 0;
constexpr int kFreshnessDotRadius = 3;
constexpr int kFreshnessDotInset = 6;

extern uint16_t gColorBackground;
extern uint16_t gColorGrid;
extern uint16_t gColorLabel;
extern uint16_t gColorCenter;
extern uint16_t gColorAircraft;
extern uint16_t gColorMilitary;
extern uint16_t gColorTrackVector;
extern uint16_t gColorTagType;
extern uint16_t gColorTagAltitude;
extern uint16_t gColorRunway;
extern uint16_t gColorRunwayLabel;
extern uint16_t gColorTagPalette[kTagPaletteCount];
extern uint16_t gColorSelect;

/** Sweep line constants. */
constexpr float kSweepDegreesPerSec = 60.0f;
constexpr int kSweepTrailCount = 3;
constexpr float kSweepTrailSpacingDeg = 4.0f;
constexpr uint8_t kSweepLeadingGreen = 200;

/** Menu overlay colors (RGB targets, converted at runtime). */
constexpr uint8_t kMenuBgR = 6, kMenuBgG = 12, kMenuBgB = 30;
constexpr uint8_t kMenuSelectedR = 0, kMenuSelectedG = 200, kMenuSelectedB = 0;
constexpr uint8_t kMenuDimR = 120, kMenuDimG = 120, kMenuDimB = 120;
constexpr uint8_t kMenuValueR = 255, kMenuValueG = 255, kMenuValueB = 255;
constexpr uint8_t kMenuHintR = 70, kMenuHintG = 70, kMenuHintB = 70;
constexpr int kMenuRingRadius = 108;
constexpr int kMenuRingThickness = 2;

}  // namespace ui::radar
