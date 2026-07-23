// Military ICAO hex ranges from https://github.com/wiedehopf/tar1090-db
// (ranges.json, "military"). Sorted by start.
#pragma once

#include <cstddef>
#include <cstdint>

namespace data::military {

struct HexRange {
  uint32_t start;
  uint32_t end;
};

constexpr HexRange kRanges[] = {
    {0x010070, 0x01008f},
    {0x0a4000, 0x0a4fff},
    {0x33ff00, 0x33ffff},
    {0x350000, 0x37ffff},
    {0x3aa000, 0x3affff},
    {0x3b7000, 0x3bffff},
    {0x3ea000, 0x3ebfff},
    {0x3f4000, 0x3fbfff},
    {0x400000, 0x40003f},
    {0x43c000, 0x43cfff},
    {0x444000, 0x446fff},
    {0x44f000, 0x44ffff},
    {0x457000, 0x457fff},
    {0x45f400, 0x45f4ff},
    {0x468000, 0x4683ff},
    {0x473c00, 0x473c0f},
    {0x478100, 0x4781ff},
    {0x480000, 0x480fff},
    {0x48d800, 0x48d87f},
    {0x497c00, 0x497cff},
    {0x498420, 0x49842f},
    {0x4b7000, 0x4b7fff},
    {0x4b8200, 0x4b82ff},
    {0x70c070, 0x70c07f},
    {0x710258, 0x71028f},
    {0x710380, 0x71039f},
    {0x738a00, 0x738aff},
    {0x7cf800, 0x7cfaff},
    {0x800200, 0x8002ff},
    {0xadf7c8, 0xafffff},
    {0xc20000, 0xc3ffff},
    {0xc87f00, 0xc87fff},
    {0xe40000, 0xe41fff},
};

constexpr size_t kRangeCount = sizeof(kRanges) / sizeof(kRanges[0]);

inline bool isMilitary(uint32_t hex) {
  for (size_t i = 0; i < kRangeCount; ++i) {
    // Ranges are sorted by start; once a start is past hex, none can match.
    if (hex < kRanges[i].start) {
      break;
    }
    if (hex <= kRanges[i].end) {
      return true;
    }
  }
  return false;
}

}  // namespace data::military
