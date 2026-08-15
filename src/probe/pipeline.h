#pragma once

#include <optional>
#include <string>
#include <vector>

#include "probe/capture.h"
#include "probe/drm.h"
#include "probe/plugins.h"

namespace subtitler::probe {

struct PipelinePlan {
  std::string device_path;
  std::string capture_format;  // V4L2 fourcc: "YUYV" or "MJPG"
  int width = 0;
  int height = 0;
  int frame_rate = 0;
  bool needs_jpegdec = false;

  std::string converter;   // "pispconvert" or "videoconvert"
  std::string kms_format;  // "NV12" or "NV16"

  std::optional<int> connector_id;

  std::vector<std::string> notes;

  bool negotiation_tested = false;
  bool negotiation_ok = false;
  std::string negotiation_error;
};

// Recommends the capture/conversion/output pipeline per docs/video-output.md
// and the #365 spike results.
PipelinePlan RecommendPipeline(const std::vector<VideoDevice>& devices,
                               const std::vector<std::vector<VideoMode>>& modes,
                               const std::vector<ElementAvailability>& elements,
                               const DrmInfo& drm);

// Drives the recommended minimal capture-to-output pipeline to PAUSED and
// records whether caps negotiated. No-op without a vc4 DRM connector.
void TestNegotiation(PipelinePlan& plan);

}  // namespace subtitler::probe
