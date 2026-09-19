#include "stream/bus.h"

#include <gst/gst.h>

#include "utils/logging.h"

namespace subtitler {

bool WaitForPipelineStartup(GstView<GstElement> pipeline, GstView<GstBus> bus,
                             GstClockTime timeout) {
  GstState current = GST_STATE_NULL;
  GstState pending = GST_STATE_VOID_PENDING;
  const auto result = gst_element_get_state(pipeline, &current, &pending,
                                             timeout);
  const auto health = PollBus(bus, pipeline, "output startup");
  return (result == GST_STATE_CHANGE_SUCCESS ||
          result == GST_STATE_CHANGE_NO_PREROLL) &&
         current == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING &&
         health == BusHealth::kOk;
}

namespace {

void PrintBusError(std::string_view pipeline_name, MessagePtr& message) {
  ErrorPtr error;
  CharPtr debug;

  gst_message_parse_error(message.get(), std::out_ptr(error),
                          std::out_ptr(debug));

  STREAM_LOG(LogLevel::kError, "{} pipeline error from {}: {}", pipeline_name,
             GST_OBJECT_NAME(message->src),
             error != nullptr ? error->message : "unknown error");

  if (debug != nullptr) {
    STREAM_LOG(LogLevel::kError, "Debug information: {}", debug.get());
  }
}

}  // namespace

BusHealth PollBus(GstView<GstBus> bus, GstView<GstElement> pipeline,
                  std::string_view pipeline_name) {
  if (bus == nullptr) {
    return BusHealth::kOk;
  }

  auto health = BusHealth::kOk;

  while (auto message = MessagePtr{gst_bus_pop(bus)}) {
    switch (GST_MESSAGE_TYPE(message.get())) {
      case GST_MESSAGE_ERROR:
        PrintBusError(pipeline_name, message);
        health = BusHealth::kError;
        break;
      case GST_MESSAGE_EOS:
        STREAM_LOG(LogLevel::kInfo, "{} pipeline reached EOS", pipeline_name);
        if (health == BusHealth::kOk) {
          health = BusHealth::kDegraded;
        }
        break;
      case GST_MESSAGE_LATENCY:
        // A sink (re)negotiated its latency; redistribute the new global
        // latency. With automatic latency this is the only way the
        // pipeline learns about it (#437).
        if (!gst_bin_recalculate_latency(GST_BIN(pipeline))) {
          STREAM_LOG(LogLevel::kError,
                     "Could not recalculate {} pipeline latency",
                     pipeline_name);
          if (health == BusHealth::kOk) {
            health = BusHealth::kDegraded;
          }
        }
        break;
      default:
        break;
    }
  }

  return health;
}

}  // namespace subtitler
