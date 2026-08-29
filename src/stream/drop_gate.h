#pragma once

#include <gst/gstelement.h>

#include <atomic>
#include <cstdint>

#include "stream/deleters.h"

namespace subtitler {

// State shared between a drop gate's pad probe (streaming thread) and
// the thread toggling it (any thread).
struct DropGate {
  explicit DropGate(std::uint32_t stride) : stride{stride} {}

  std::atomic_bool active = false;
  // Kept 1 buffer in stride while active (60 fps in, 10 fps out at
  // stride 6; stride 1 keeps everything). Probe-local; only the
  // streaming thread touches it.
  std::uint64_t counter = 0;
  const std::uint32_t stride;
};

// Installs a drop gate on a tee branch's queue: a pad probe that drops
// branch buffers while inactive and keeps every stride-th while active.
// A valve cannot gate a branch — while dropping it fails serialized
// queries (latency included), which stalls latency computation for the
// whole pipeline — and a videorate anchors its output cadence to the
// segment start, so after any gated gap it emits a catch-up burst. The
// probe drops only buffers (queries and sticky events keep flowing, so
// the branch stays negotiated), with no state to go stale. Used by the
// MJPEG preview branch (#379) and the whisper tap (#19).
void InstallDropGate(GstView<GstElement> queue, DropGate& gate);

}  // namespace subtitler
