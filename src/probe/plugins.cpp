#include "probe/plugins.h"

#include <gst/gst.h>

#include "stream/deleters.h"

namespace subtitler::probe {

std::vector<ElementAvailability> ProbeElements() {
  gst_init(nullptr, nullptr);

  static const char* const elements[] = {
      "v4l2src", "appsink",  "appsrc",       "textoverlay",
      "alsasrc", "alsasink", "kmssink",      "pispconvert",
      "jpegenc", "jpegdec",  "videoconvert", "v4l2convert",
  };

  std::vector<ElementAvailability> result;
  for (const char* name : elements) {
    GstPointer<GstElementFactory> factory{gst_element_factory_find(name)};
    result.push_back({name, factory != nullptr});
  }
  return result;
}

}  // namespace subtitler::probe
