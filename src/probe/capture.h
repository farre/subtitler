#pragma once

#include <string>
#include <vector>

namespace subtitler::probe {

struct VideoMode {
  std::string format;  // V4L2 fourcc, e.g. "YUYV"
  int width;
  int height;
  std::vector<int> frame_rates;
};

struct VideoDevice {
  std::string path;
  std::string card;
  std::string driver;
  std::string bus;
  bool is_cv105 = false;
};

// Lists video capture devices (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING,
// metadata and mem2mem nodes excluded).
std::vector<VideoDevice> ListVideoDevices();

std::vector<VideoMode> ListCaptureModes(const std::string& path);

}  // namespace subtitler::probe
