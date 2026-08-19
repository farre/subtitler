#include "stream/preview_gate.h"

#include <gst/gstpad.h>

namespace {

GstPadProbeReturn PreviewGateProbe(GstPad*, GstPadProbeInfo*,
                                   gpointer user_data) {
  const auto& active = *static_cast<std::atomic_bool*>(user_data);

  return active.load(std::memory_order_relaxed) ? GST_PAD_PROBE_OK
                                                : GST_PAD_PROBE_DROP;
}

}  // namespace

namespace subtitler {

void InstallPreviewGate(GstView<GstElement> preview_queue,
                        std::atomic_bool& active) {
  PadPtr pad{gst_element_get_static_pad(preview_queue, "src")};

  gst_pad_add_probe(pad.get(), GST_PAD_PROBE_TYPE_BUFFER, PreviewGateProbe,
                    &active, nullptr);
}

}  // namespace subtitler
