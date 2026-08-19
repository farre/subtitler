#pragma once

#include <gst/gstelement.h>

#include <atomic>

#include "stream/deleters.h"

namespace subtitler {

// Installs the preview gate on the preview branch's queue: a pad probe
// that drops branch buffers while active is false. A valve cannot do
// this — while dropping it fails serialized queries (latency included),
// which stalls latency computation for the whole pipeline, HDMI branch
// included. Dropping buffers in a probe leaves queries and sticky events
// (caps, segment) flowing, so the branch stays negotiated and starts
// immediately when activated.
void InstallPreviewGate(GstView<GstElement> preview_queue,
                        std::atomic_bool& active);

}  // namespace subtitler
