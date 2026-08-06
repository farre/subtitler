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

std::string output_pipeline_description(std::optional<int> connector_id) {
  const auto connector = connector_id
                             ? std::format(" connector-id={}", *connector_id)
                             : std::string{};

  return std::format(
      "appsrc "
      "name=output_source "
      "is-live=true "
      "format=time "
      "block=true "
      "max-buffers=2 "
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
      width, height, frames_per_second, connector);
}
}  // namespace subtitler
