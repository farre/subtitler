#pragma once

#include <cstdint>
#include <string_view>

#include "stream/deleters.h"

namespace subtitler {

// The health of a pipeline after draining its bus.
enum class BusHealth : std::uint8_t {
  kOk,
  // The pipeline can't continue, but the device isn't necessarily
  // dead: EOS or a failed latency redistribution. On the capture side
  // this is the intended no-signal fallback (the HDMI source went
  // away); on the output side it's fatal like any output death.
  kDegraded,
  // A GST_MESSAGE_ERROR arrived: the device/pipeline is dead. On the
  // capture side this fails the process so the service manager
  // restarts the appliance — the restart path is how a re-enumerating
  // capture device comes back; the pink screensaver is only for the
  // no-signal state, never for a dead device.
  kError,
};

// Drains the pipeline's bus, logging errors, and answers the pipeline's
// health. A null bus answers kOk (that side isn't running).
BusHealth PollBus(GstView<GstBus> bus, GstView<GstElement> pipeline,
                  std::string_view pipeline_name);

}  // namespace subtitler
