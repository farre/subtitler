#pragma once

#include <gst/gst.h>

#include <optional>

namespace subtitler {

// Maps capture-domain timestamps onto the output pipeline's running time.
// Sinks render at PTS + the pipeline's configured latency, so the anchor
// subtracts that latency to keep the end-to-end delay at the target
// latency. Re-anchors on timestamp discontinuities and latency
// reconfiguration.
class OutputAnchor {
 public:
  explicit OutputAnchor(GstClockTime target_latency)
      : target_latency_{target_latency} {}

  GstClockTime Map(GstClockTime capture_pts, GstClockTime output_now,
                   GstClockTime pipeline_latency);

 private:
  const GstClockTime target_latency_;

  std::optional<GstClockTime> capture_;
  std::optional<GstClockTime> output_;
  GstClockTime previous_capture_pts_ = GST_CLOCK_TIME_NONE;
  GstClockTime latency_ = 0;
};

}  // namespace subtitler
