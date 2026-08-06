#include "probe/capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "probe/fd.h"

namespace {

int xioctl(int fd, unsigned long request, void* arg) {
  int result;
  do {
    result = ioctl(fd, request, arg);
  } while (result == -1 && errno == EINTR);
  return result;
}

std::string fourcc_to_string(std::uint32_t fourcc) {
  std::string result(4, '\0');
  for (int i = 0; i < 4; ++i) {
    result[i] = static_cast<char>((fourcc >> (8 * i)) & 0xff);
  }
  return result;
}

// The CV105 identifies as UltraSemi "USB3 Video", VID:PID 345f:2130
// (docs/pi-setup.md).
bool is_cv105(const std::filesystem::path& device_name) {
  std::ifstream modalias{"/sys/class/video4linux" / device_name / "device" /
                         "modalias"};
  std::string contents;
  std::getline(modalias, contents);
  return contents.find("v345Fp2130") != std::string::npos;
}

}  // namespace

namespace subtitler::probe {

std::vector<VideoDevice> list_video_devices() {
  std::vector<VideoDevice> devices;

  for (const auto& entry : std::filesystem::directory_iterator{"/dev"}) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with("video")) {
      continue;
    }

    const Fd fd{open(entry.path().c_str(), O_RDWR | O_CLOEXEC)};
    if (!fd) {
      continue;
    }

    v4l2_capability caps{};
    if (xioctl(fd.get(), VIDIOC_QUERYCAP, &caps) != 0) {
      continue;
    }

    const auto device_caps = (caps.capabilities & V4L2_CAP_DEVICE_CAPS)
                                 ? caps.device_caps
                                 : caps.capabilities;
    constexpr auto wanted = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    if ((device_caps & wanted) != wanted) {
      continue;
    }

    devices.push_back(VideoDevice{
        .path = entry.path().string(),
        .card = reinterpret_cast<const char*>(caps.card),
        .driver = reinterpret_cast<const char*>(caps.driver),
        .bus = reinterpret_cast<const char*>(caps.bus_info),
        .is_cv105 = is_cv105(name),
    });
  }

  return devices;
}

std::vector<VideoMode> list_capture_modes(const std::string& path) {
  const Fd fd{open(path.c_str(), O_RDWR | O_CLOEXEC)};
  if (!fd) {
    return {};
  }

  std::vector<VideoMode> modes;

  v4l2_fmtdesc format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  while (xioctl(fd.get(), VIDIOC_ENUM_FMT, &format) == 0) {
    v4l2_frmsizeenum size{};
    size.pixel_format = format.pixelformat;
    while (xioctl(fd.get(), VIDIOC_ENUM_FRAMESIZES, &size) == 0) {
      if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
        ++size.index;
        continue;
      }

      VideoMode mode{
          .format = fourcc_to_string(format.pixelformat),
          .width = static_cast<int>(size.discrete.width),
          .height = static_cast<int>(size.discrete.height),
          .frame_rates = {},
      };

      v4l2_frmivalenum interval{};
      interval.pixel_format = format.pixelformat;
      interval.width = size.discrete.width;
      interval.height = size.discrete.height;
      while (xioctl(fd.get(), VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0) {
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE &&
            interval.discrete.numerator != 0) {
          mode.frame_rates.push_back(static_cast<int>(
              interval.discrete.denominator / interval.discrete.numerator));
        }
        ++interval.index;
      }

      modes.push_back(std::move(mode));
      ++size.index;
    }
    ++format.index;
  }

  return modes;
}

}  // namespace subtitler::probe
