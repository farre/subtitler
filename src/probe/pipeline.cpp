#include "probe/pipeline.h"

#include <gst/gst.h>

#include <format>
#include <fstream>
#include <memory>
#include <string_view>

#include "stream/deleters.h"

namespace {

template <typename T>
using GstPointer = std::unique_ptr<T, subtitler::GstDeleter<T>>;

constexpr int width = 1920;
constexpr int height = 1080;
constexpr int frames_per_second = 60;

bool has_mode(const std::vector<subtitler::probe::VideoMode>& modes,
              std::string_view format) {
  for (const auto& mode : modes) {
    if (mode.format != format || mode.width != width || mode.height != height) {
      continue;
    }
    for (const int rate : mode.frame_rates) {
      if (rate == frames_per_second) {
        return true;
      }
    }
  }
  return false;
}

bool has_element(
    const std::vector<subtitler::probe::ElementAvailability>& elements,
    std::string_view name) {
  for (const auto& element : elements) {
    if (element.name == name) {
      return element.available;
    }
  }
  return false;
}

// raspberrypi/libpisp#76 (NV12 output renders blue) affects only the
// BCM2712C1 stepping, which ships in Pi 5 family boards marked "Rev 1.1".
bool is_bcm2712c1() {
  std::ifstream cpuinfo{"/proc/cpuinfo"};
  if (!cpuinfo) {
    return false;
  }

  const std::string contents{std::istreambuf_iterator<char>{cpuinfo},
                             std::istreambuf_iterator<char>{}};

  const bool pi5_family = contents.contains("Raspberry Pi 5") ||
                          contents.contains("Raspberry Pi 500") ||
                          contents.contains("Compute Module 5");

  return pi5_family && contents.contains("Rev 1.1");
}

}  // namespace

namespace subtitler::probe {

PipelinePlan recommend_pipeline(
    const std::vector<VideoDevice>& devices,
    const std::vector<std::vector<VideoMode>>& modes,
    const std::vector<ElementAvailability>& elements, const DrmInfo& drm) {
  PipelinePlan plan;

  std::optional<std::size_t> chosen;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (!has_mode(modes[i], "YUYV") && !has_mode(modes[i], "MJPG")) {
      continue;
    }
    if (devices[i].is_cv105) {
      chosen = i;
      break;
    }
    if (!chosen) {
      chosen = i;
    }
  }

  if (!chosen) {
    plan.notes.emplace_back("no capture device offers 1920x1080 at 60 fps");
    return plan;
  }

  plan.device_path = devices[*chosen].path;
  plan.width = width;
  plan.height = height;
  plan.frame_rate = frames_per_second;

  if (has_mode(modes[*chosen], "YUYV")) {
    plan.capture_format = "YUYV";
  } else {
    plan.capture_format = "MJPG";
    plan.needs_jpegdec = true;
    if (!has_element(elements, "jpegdec")) {
      plan.notes.emplace_back(
          "MJPG capture requires jpegdec, which is not installed");
    }
  }

  const bool pisp = has_element(elements, "pispconvert");
  if (pisp && !is_bcm2712c1()) {
    plan.converter = "pispconvert";
    plan.kms_format = "NV12";
  } else {
    plan.converter = "videoconvert";
    plan.kms_format = "NV16";
    if (pisp) {
      plan.notes.emplace_back(
          "BCM2712C1 detected: pispconvert NV12 output renders blue "
          "(raspberrypi/libpisp#76), recommending software conversion");
    }
  }

  if (!has_element(elements, plan.converter)) {
    plan.notes.push_back(
        std::format("converter {} is not installed", plan.converter));
  }
  if (!has_element(elements, "kmssink")) {
    plan.notes.emplace_back("kmssink is not installed");
  }

  for (const auto& connector : drm.connectors) {
    if (connector.connected) {
      plan.connector_id = connector.id;
      break;
    }
  }
  if (!plan.connector_id) {
    plan.notes.emplace_back("no connected vc4 DRM connector");
  }

  return plan;
}

void test_negotiation(PipelinePlan& plan) {
  if (plan.device_path.empty() || plan.converter.empty()) {
    return;
  }
  if (!plan.connector_id) {
    plan.notes.emplace_back("negotiation test skipped: no vc4 DRM connector");
    return;
  }

  plan.negotiation_tested = true;

  const std::string source =
      plan.capture_format == "MJPG"
          ? std::format(
                "v4l2src device=\"{}\" ! image/jpeg,width={},height={},"
                "framerate={}/1 ! jpegdec",
                plan.device_path, plan.width, plan.height, plan.frame_rate)
          : std::format(
                "v4l2src device=\"{}\" ! video/x-raw,format=YUY2,"
                "width={},height={},framerate={}/1",
                plan.device_path, plan.width, plan.height, plan.frame_rate);

  const std::string conversion =
      plan.converter == "pispconvert"
          ? std::format(
                "pispconvert ! "
                "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format="
                "{},width={},height={},framerate={}/1",
                plan.kms_format, plan.width, plan.height, plan.frame_rate)
          : std::format(
                "videoconvert ! video/x-raw,format={},width={},"
                "height={},framerate={}/1",
                plan.kms_format, plan.width, plan.height, plan.frame_rate);

  const std::string description =
      std::format("{} ! {} ! kmssink driver-name=vc4 connector-id={}", source,
                  conversion, *plan.connector_id);

  gst_init(nullptr, nullptr);

  GstPointer<GError> error;
  const GstPointer<GstElement> pipeline{
      gst_parse_launch(description.c_str(), std::out_ptr(error))};

  if (pipeline == nullptr) {
    plan.negotiation_error =
        std::format("could not build pipeline: {}",
                    error != nullptr ? error->message : "unknown error");
    return;
  }

  if (gst_element_set_state(pipeline.get(), GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    plan.negotiation_error = "pipeline failed to reach PAUSED";
  } else {
    const auto result =
        gst_element_get_state(pipeline.get(), nullptr, nullptr, 5 * GST_SECOND);
    if (result == GST_STATE_CHANGE_FAILURE) {
      plan.negotiation_error = "pipeline failed to reach PAUSED";
    } else if (result == GST_STATE_CHANGE_ASYNC) {
      plan.negotiation_error = "timed out waiting for PAUSED";
    } else {
      plan.negotiation_ok = true;
    }
  }

  if (!plan.negotiation_ok) {
    const GstPointer<GstBus> bus{gst_element_get_bus(pipeline.get())};
    if (bus != nullptr) {
      const GstPointer<GstMessage> message{
          gst_bus_timed_pop_filtered(bus.get(), GST_SECOND, GST_MESSAGE_ERROR)};
      if (message != nullptr) {
        GstPointer<GError> bus_error;
        GstPointer<gchar> debug;
        gst_message_parse_error(message.get(), std::out_ptr(bus_error),
                                std::out_ptr(debug));
        if (bus_error != nullptr) {
          plan.negotiation_error = bus_error->message;
        }
      }
    }
  }

  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
}

}  // namespace subtitler::probe
