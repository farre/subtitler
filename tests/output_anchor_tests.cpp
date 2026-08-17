#include <doctest/doctest.h>
#include <gst/gst.h>

#include "stream/output_anchor.h"

namespace {

constexpr GstClockTime kTarget = 50 * GST_MSECOND;

}  // namespace

TEST_CASE("output anchor") {
  subtitler::OutputAnchor anchor{kTarget};

  SUBCASE("maps capture time onto output time") {
    // With the latency pinned to the target, the head start is zero and
    // the mapping is the identity shifted to the first frame.
    CHECK(anchor.Map(1000 * GST_MSECOND, 2000 * GST_MSECOND,
                     50 * GST_MSECOND) == 2000 * GST_MSECOND);
    CHECK(anchor.Map(1016 * GST_MSECOND, 2016 * GST_MSECOND,
                     50 * GST_MSECOND) == 2016 * GST_MSECOND);
    CHECK(anchor.Map(1033 * GST_MSECOND, 2033 * GST_MSECOND,
                     50 * GST_MSECOND) == 2033 * GST_MSECOND);
  }

  SUBCASE("compensates the configured latency") {
    // Render is PTS + latency: with latency 230 the anchor must start
    // 180 ms before the output time so rendering lands on now + target.
    CHECK(anchor.Map(1000 * GST_MSECOND, 2000 * GST_MSECOND,
                     230 * GST_MSECOND) == 1820 * GST_MSECOND);
  }

  SUBCASE("clamps the anchor at zero") {
    CHECK(anchor.Map(1000 * GST_MSECOND, 100 * GST_MSECOND,
                     500 * GST_MSECOND) == 0);
  }

  SUBCASE("treats invalid latency as zero") {
    CHECK(anchor.Map(1000 * GST_MSECOND, 2000 * GST_MSECOND,
                     GST_CLOCK_TIME_NONE) == 2050 * GST_MSECOND);
  }

  SUBCASE("re-anchors on a capture timestamp discontinuity") {
    anchor.Map(1000 * GST_MSECOND, 2000 * GST_MSECOND, 50 * GST_MSECOND);
    // Capture time moves backwards (restart, seek, screensaver): the
    // anchor must restart from the new capture time.
    CHECK(anchor.Map(500 * GST_MSECOND, 2100 * GST_MSECOND, 50 * GST_MSECOND) ==
          2100 * GST_MSECOND);
    CHECK(anchor.Map(516 * GST_MSECOND, 2116 * GST_MSECOND, 50 * GST_MSECOND) ==
          2116 * GST_MSECOND);
  }

  SUBCASE("re-anchors on a latency change") {
    anchor.Map(1000 * GST_MSECOND, 2000 * GST_MSECOND, 50 * GST_MSECOND);
    CHECK(anchor.Map(1016 * GST_MSECOND, 2016 * GST_MSECOND,
                     90 * GST_MSECOND) == 1976 * GST_MSECOND);
    CHECK(anchor.Map(1033 * GST_MSECOND, 2033 * GST_MSECOND,
                     90 * GST_MSECOND) == 1993 * GST_MSECOND);
  }
}
