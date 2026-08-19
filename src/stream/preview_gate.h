#pragma once

#include <gst/gstelement.h>

#include <atomic>
#include <cstdint>

#include "stream/deleters.h"

namespace subtitler {

// State shared between the preview gate's pad probe (streaming thread)
// and Stream::SetPreviewActive (any thread).
struct PreviewGate {
  explicit PreviewGate(std::uint32_t stride) : stride{stride} {}

  std::atomic_bool active = false;
  // Kept 1 frame in stride when active (60 fps in, 10 fps out at stride 6).
  // Probe-local; only the streaming thread touches it.
  std::uint64_t counter = 0;
  const std::uint32_t stride;
};

// Installs the preview gate on the preview branch's queue: a pad probe
// that drops branch buffers while inactive and decimates them while
// active. A valve cannot gate the branch — while dropping it fails
// serialized queries (latency included), which stalls latency computation
// for the whole pipeline, HDMI branch included — and a videorate anchors
// its output cadence to the segment start, so after any gated gap it
// emits a catch-up burst. The probe drops only buffers (queries and
// sticky events keep flowing, so the branch stays negotiated) and keeps
// exactly every stride-th frame, with no state to go stale.
void InstallPreviewGate(GstView<GstElement> preview_queue,
                        PreviewGate& gate);

}  // namespace subtitler
