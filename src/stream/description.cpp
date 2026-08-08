#include "stream/description.h"

#include <format>

constexpr int width = 1920;
constexpr int height = 1080;
constexpr int frames_per_second = 60;

namespace subtitler {
std::string capture_pipeline_description(std::string_view device) {
  return std::format(
      "v4l2src "
      "device=\"{}\" "
      "io-mode=mmap "
      "do-timestamp=true "
      "! video/x-raw,"
      "format=YUY2,"
      "width={},"
      "height={},"
      "framerate={}/1 "
      "! appsink "
      "name=capture_sink "
      "sync=false "
      "max-buffers=2 "
      "drop=true",
      device, width, height, frames_per_second);
}

std::string output_pipeline_description(OutputMode mode,
                                        std::optional<int> connector_id) {
  const auto connector = connector_id
                             ? std::format(" connector-id={}", *connector_id)
                             : std::string{};

  const std::string base = std::format(
      "appsrc "
      "name=output_source "
      "is-live=true "
      "format=time "
      "block=true "
      "max-buffers=2 ");

  switch (mode) {
    case OutputMode::kKmsPisp:
      return std::format(
          "{}"
          "! pispconvert "
          "name=converter "
          "output-buffer-count=4 "
          "! video/x-raw(memory:DMABuf),"
          "format=DMA_DRM,"
          "drm-format=NV12,"
          "width={},"
          "height={},"
          "framerate={}/1 "
          "! kmssink "
          "driver-name=vc4 "
          "force-modesetting=true "
          "sync=true"
          "{}",
          base, width, height, frames_per_second, connector);

    case OutputMode::kKmsSoftware:
      return std::format(
          "{}"
          "! videoconvert "
          "n-threads=4 "
          "! video/x-raw,"
          "format=NV16,"
          "width={},"
          "height={},"
          "framerate={}/1 "
          "! kmssink "
          "driver-name=vc4 "
          "force-modesetting=true "
          "sync=true"
          "{}",
          base, width, height, frames_per_second, connector);

    case OutputMode::kWindow:
      return std::format(
          "{}"
          "! videoconvert "
          "n-threads=4 "
          "! glimagesink sync=true",
          base);

    case OutputMode::kNull:
      return std::format("{}! fakesink sync=true", base);
  }

  return "";
}
}  // namespace subtitler
