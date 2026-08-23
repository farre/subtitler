#include "stream/fonts.h"

#include <algorithm>

#include <doctest/doctest.h>

// The families the subtitle renderer can use (#159), straight from
// pango's font map. Needs fonts installed (the Pi target installs
// them).
TEST_CASE("available font families") {
  const auto families = subtitler::AvailableFontFamilies();

  INFO("enumerated ", families.size(), " families");
  CHECK_FALSE(families.empty());
  for (const auto& family : families) {
    CHECK_FALSE(family.empty());
  }
  CHECK(std::ranges::is_sorted(families));
}
