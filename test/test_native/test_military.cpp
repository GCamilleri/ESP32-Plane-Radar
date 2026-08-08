#include <doctest.h>

#include "data/military_ranges.h"

using data::military::isMilitary;

TEST_CASE("hex on the edges of a military range is military") {
  // First range in the table: {0x010070, 0x01008f}.
  CHECK(isMilitary(0x010070));        // start edge
  CHECK(isMilitary(0x01008f));        // end edge
  CHECK(isMilitary(0x010080));        // inside
  CHECK_FALSE(isMilitary(0x01006f));  // just below
  CHECK_FALSE(isMilitary(0x010090));  // just above
}

TEST_CASE("civilian hex is not military") {
  CHECK_FALSE(isMilitary(0x000000));
  CHECK_FALSE(isMilitary(0x4CA000));  // typical civil registration block
  CHECK_FALSE(isMilitary(0xFFFFFF));  // above the last range
}

TEST_CASE("large blocks and their boundaries") {
  CHECK(isMilitary(0xadf7c8));        // start of {0xadf7c8, 0xafffff}
  CHECK(isMilitary(0xae1234));        // inside
  CHECK(isMilitary(0xafffff));        // end
  CHECK_FALSE(isMilitary(0xadf7c7));  // one below the start
  CHECK(isMilitary(0xc20000));        // {0xc20000, 0xc3ffff}
  CHECK(isMilitary(0xc3ffff));
  CHECK_FALSE(isMilitary(0xc40000));  // one above the end
}
