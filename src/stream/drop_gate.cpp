#include "stream/drop_gate.h"

#include <gst/gstpad.h>

namespace {

GstPadProbeReturn DropGateProbe(GstPad*, GstPadProbeInfo*, gpointer user_data) {
  auto& gate = *static_cast<subtitler::DropGate*>(user_data);

  if (!gate.active.load(std::memory_order_relaxed)) {
    return GST_PAD_PROBE_DROP;
  }

  return gate.counter++ % gate.stride == 0 ? GST_PAD_PROBE_OK
                                           : GST_PAD_PROBE_DROP;
}

}  // namespace

namespace subtitler {

void InstallDropGate(GstView<GstElement> queue, DropGate& gate) {
  PadPtr pad{gst_element_get_static_pad(queue, "src")};

  gst_pad_add_probe(pad.get(), GST_PAD_PROBE_TYPE_BUFFER, DropGateProbe, &gate,
                    nullptr);
}

}  // namespace subtitler
