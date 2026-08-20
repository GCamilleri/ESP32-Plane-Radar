#include <doctest.h>

#include <cstring>

#include "ui/text_fit.h"

using ui::text::buildEllipsized;
using ui::text::kEllipsis;

TEST_CASE("ASCII truncation appends the ellipsis at the budget") {
  char buf[32];
  CHECK(buildEllipsized(buf, sizeof(buf), "MyNetwork", 2) == 2);
  CHECK(std::strcmp(buf, "My\xE2\x80\xA6") == 0);
}

TEST_CASE("a budget at or past the source length copies all of it") {
  char buf[32];
  CHECK(buildEllipsized(buf, sizeof(buf), "abc", 3) == 3);
  CHECK(std::strcmp(buf, "abc\xE2\x80\xA6") == 0);
  // Over-long budgets clamp to the source rather than reading past it.
  CHECK(buildEllipsized(buf, sizeof(buf), "abc", 99) == 3);
  CHECK(std::strcmp(buf, "abc\xE2\x80\xA6") == 0);
}

TEST_CASE("a cut inside a multi-byte character snaps back to its start") {
  char buf[32];
  // "café" -- the e-acute is the 2 bytes 0xC3 0xA9 at offsets 3 and 4.
  const char* src = "caf\xC3\xA9";
  // Budget 4 would split the e-acute, so it must drop the whole character.
  CHECK(buildEllipsized(buf, sizeof(buf), src, 4) == 3);
  CHECK(std::strcmp(buf, "caf\xE2\x80\xA6") == 0);
  // Budget 5 lands on the boundary after it, so the character survives whole.
  CHECK(buildEllipsized(buf, sizeof(buf), src, 5) == 5);
  CHECK(std::strcmp(buf, "caf\xC3\xA9\xE2\x80\xA6") == 0);
}

TEST_CASE("a 4-byte character is dropped whole from any interior cut") {
  char buf[32];
  // U+1F6EB airplane departure: 0xF0 0x9F 0x9B 0xAB.
  const char* src = "A\xF0\x9F\x9B\xAB";
  for (size_t budget = 2; budget <= 4; ++budget) {
    CHECK(buildEllipsized(buf, sizeof(buf), src, budget) == 1);
    CHECK(std::strcmp(buf, "A\xE2\x80\xA6") == 0);
  }
  CHECK(buildEllipsized(buf, sizeof(buf), src, 5) == 5);
}

TEST_CASE("a zero budget yields a bare ellipsis") {
  char buf[32];
  CHECK(buildEllipsized(buf, sizeof(buf), "anything", 0) == 0);
  CHECK(std::strcmp(buf, kEllipsis) == 0);
}

TEST_CASE("the budget is clamped to what the buffer can hold") {
  // Room for the ellipsis (3 bytes), the NUL, and exactly 2 source bytes.
  char buf[6];
  CHECK(buildEllipsized(buf, sizeof(buf), "abcdef", 6) == 2);
  CHECK(std::strcmp(buf, "ab\xE2\x80\xA6") == 0);
  CHECK(std::strlen(buf) == sizeof(buf) - 1);
}

TEST_CASE("a buffer too small for even the ellipsis is rejected") {
  char buf[8];
  CHECK(buildEllipsized(buf, 3, "abc", 1) == -1);
  CHECK(buildEllipsized(buf, 0, "abc", 1) == -1);
  CHECK(buildEllipsized(nullptr, sizeof(buf), "abc", 1) == -1);
}

TEST_CASE("a null source is treated as empty") {
  char buf[32];
  CHECK(buildEllipsized(buf, sizeof(buf), nullptr, 5) == 0);
  CHECK(std::strcmp(buf, kEllipsis) == 0);
}

TEST_CASE("the shrink loop used for SSID fitting terminates at the ellipsis") {
  // Mirrors fitSsidLine(): each pass starts inside the last surviving character
  // so a too-wide string always converges on a bare ellipsis.
  char buf[32];
  const char* src = "caf\xC3\xA9\xC3\xA9";
  size_t budget = std::strlen(src);
  int passes = 0;
  for (;;) {
    const int copied = buildEllipsized(buf, sizeof(buf), src, budget);
    REQUIRE(copied >= 0);
    if (copied == 0) break;
    budget = static_cast<size_t>(copied) - 1;
    REQUIRE(++passes < 16);
  }
  CHECK(std::strcmp(buf, kEllipsis) == 0);
}
