#include "probe/plugins.h"

#include <gst/gst.h>

#include <memory>

#include "stream/deleters.h"

namespace {

template <typename T>
using GstPointer = std::unique_ptr<T, subtitler::GstDeleter<T>>;

}  // namespace

namespace subtitler::probe {

std::vector<ElementAvailability> probe_elements() {
  gst_init(nullptr, nullptr);

  static const char* const elements[] = {
      "v4l2src",     "appsink", "appsrc",  "textoverlay",
      "pispconvert", "kmssink", "jpegenc", "videoconvert",
  };

  std::vector<ElementAvailability> result;
  for (const char* name : elements) {
    GstPointer<GstElementFactory> factory{gst_element_factory_find(name)};
    result.push_back({name, factory != nullptr});
  }
  return result;
}

}  // namespace subtitler::probe
