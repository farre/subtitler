#include <doctest/doctest.h>
#include <gst/gst.h>

#include <string_view>

#include "stream/bus.h"
#include "stream/deleters.h"

namespace {

using namespace subtitler;

// A minimal live pipeline to own a bus; the messages are posted by the
// test, so no device is involved.
struct PipelineRig {
  PipelineRig() {
    pipeline = ElementPtr{gst_pipeline_new("test")};
    REQUIRE(pipeline != nullptr);
    bus = BusPtr{gst_element_get_bus(pipeline.get())};
    REQUIRE(bus != nullptr);
  }
  ~PipelineRig() { gst_element_set_state(pipeline.get(), GST_STATE_NULL); }

  ElementPtr pipeline;
  BusPtr bus;
};

void PostError(GstView<GstElement> element) {
  GError* error = g_error_new_literal(GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
                                      "injected failure");
  gst_element_post_message(
      element, gst_message_new_error(GST_OBJECT(element), error, "injected"));
}

}  // namespace

TEST_CASE("poll bus classifies pipeline health") {
  gst_init(nullptr, nullptr);

  SUBCASE("an empty bus is healthy") {
    PipelineRig rig;
    CHECK(PollBus(rig.bus.get(), rig.pipeline.get(), "test") ==
          BusHealth::kOk);
  }

  SUBCASE("a bus error is a device death") {
    PipelineRig rig;
    PostError(rig.pipeline.get());
    CHECK(PollBus(rig.bus.get(), rig.pipeline.get(), "test") ==
          BusHealth::kError);
  }

  SUBCASE("EOS degrades without an error") {
    PipelineRig rig;
    gst_element_post_message(
        rig.pipeline.get(),
        gst_message_new_eos(GST_OBJECT(rig.pipeline.get())));
    CHECK(PollBus(rig.bus.get(), rig.pipeline.get(), "test") ==
          BusHealth::kDegraded);
  }

  SUBCASE("an error wins over EOS in the same burst") {
    PipelineRig rig;
    gst_element_post_message(
        rig.pipeline.get(),
        gst_message_new_eos(GST_OBJECT(rig.pipeline.get())));
    PostError(rig.pipeline.get());
    CHECK(PollBus(rig.bus.get(), rig.pipeline.get(), "test") ==
          BusHealth::kError);
  }

  SUBCASE("a null bus is healthy (that side isn't running)") {
    PipelineRig rig;
    CHECK(PollBus(nullptr, rig.pipeline.get(), "test") == BusHealth::kOk);
  }

  SUBCASE("the bus is drained, not latched") {
    PipelineRig rig;
    PostError(rig.pipeline.get());
    CHECK(PollBus(rig.bus.get(), rig.pipeline.get(), "test") ==
          BusHealth::kError);
    CHECK(PollBus(rig.bus.get(), rig.pipeline.get(), "test") ==
          BusHealth::kOk);
  }
}
