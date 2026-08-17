#include "stream/output_anchor.h"

#include <algorithm>
#include <cstdint>

namespace subtitler {

GstClockTime OutputAnchor::Map(GstClockTime capture_pts,
                               GstClockTime output_now,
                               GstClockTime pipeline_latency) {
  if (!GST_CLOCK_TIME_IS_VALID(pipeline_latency)) {
    pipeline_latency = 0;
  }

  if (pipeline_latency != latency_) {
    latency_ = pipeline_latency;
    capture_.reset();
  }

  const bool discontinuity = GST_CLOCK_TIME_IS_VALID(previous_capture_pts_) &&
                             capture_pts < previous_capture_pts_;

  if (!capture_ || discontinuity) {
    capture_ = capture_pts;

    const auto target = static_cast<std::int64_t>(output_now) +
                        target_latency_ - static_cast<std::int64_t>(latency_);
    output_ = static_cast<GstClockTime>(std::max<std::int64_t>(target, 0));
  }

  previous_capture_pts_ = capture_pts;

  return *output_ + (capture_pts - *capture_);
}

}  // namespace subtitler
