#include <doctest/doctest.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <string>
#include <thread>

#include "stream/deleters.h"
#include "stream/preview_gate.h"
#include "stream/stream.h"

namespace {

using namespace subtitler;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFramesPerSecond = 60;
constexpr auto kFrameDuration = GST_SECOND / kFramesPerSecond;

// The main sink is unnamed in the description; it is the sink that is not
// the preview appsink.
ElementPtr FindMainSink(GstView<GstElement> bin,
                        GstView<GstElement> preview_sink) {
  IteratorPtr it{gst_bin_iterate_sinks(GST_BIN(bin))};

  GValue item = G_VALUE_INIT;

  while (gst_iterator_next(it.get(), &item) == GST_ITERATOR_OK) {
    if (GST_ELEMENT(g_value_get_object(&item)) != preview_sink) {
      ElementPtr result{GST_ELEMENT(g_value_dup_object(&item))};
      g_value_unset(&item);
      return result;
    }

    g_value_reset(&item);
  }

  g_value_unset(&item);
  return nullptr;
}

void OnHandoff(GstElement*, GstBuffer*, GstPad*, gpointer user_data) {
  ++*static_cast<std::atomic_uint*>(user_data);
}

// Pushes count YUY2 frames at 60 fps into the output appsrc, timestamped
// in the pipeline's running time like the shared capture/output domain
// produces, a little ahead so the synced sink renders instead of
// dropping. Returns the number of frames pushed.
int PushFrames(GstView<GstAppSrc> source, int count, std::uint64_t first_pts) {
  for (int i = 0; i < count; ++i) {
    BufferPtr buffer{
        gst_buffer_new_allocate(nullptr, kWidth * kHeight * 2, nullptr)};

    if (buffer == nullptr) {
      return i;
    }

    GST_BUFFER_PTS(buffer.get()) = first_pts + i * kFrameDuration;
    GST_BUFFER_DURATION(buffer.get()) = kFrameDuration;

    if (gst_app_src_push_buffer(source, buffer.release()) != GST_FLOW_OK) {
      return i;
    }
  }

  return count;
}

}  // namespace

// Regression test for the preview branch's isolation guarantee (#379): the
// HDMI branch must flow with the preview gate closed (its default state),
// and opening the gate mid-stream must start the JPEG flow without
// disturbing the HDMI branch. Exercises the real pipeline description with
// the shared-timeline configuration StartOutput applies. (The first cut
// used a valve; while dropping it fails serialized queries pipeline-wide
// and stalled the HDMI branch — the gate is a buffer-dropping pad probe.)
TEST_CASE("output pipeline preview branch") {
  gst_init(nullptr, nullptr);

  subtitler::GstPointer<GstElementFactory> jpeg_factory{
      gst_element_factory_find("jpegenc")};
  if (jpeg_factory == nullptr) {
    return;
  }

  const std::string description = subtitler::OutputPipelineDescription(
      subtitler::OutputMode::kNull, std::nullopt, std::nullopt, true);

  ErrorPtr error;
  ElementPtr pipeline{gst_parse_launch(description.c_str(), std::out_ptr(error))};
  INFO("gst_parse_launch error: ",
       error != nullptr ? std::string{error->message} : "none");
  REQUIRE(error == nullptr);

  ElementPtr source{
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "output_source")};
  ElementPtr preview_sink{
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "preview_sink")};
  ElementPtr preview_queue{
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "preview_queue")};

  REQUIRE(source != nullptr);
  REQUIRE(preview_sink != nullptr);
  REQUIRE(preview_queue != nullptr);

  ElementPtr main_sink = FindMainSink(pipeline.get(), preview_sink.get());
  REQUIRE(main_sink != nullptr);

  // The production gate (#379): drops preview branch buffers while
  // inactive and keeps every 6th frame while active.
  subtitler::PreviewGate gate{6};
  subtitler::InstallPreviewGate(preview_queue.get(), gate);

  const auto app_src = GstView<GstAppSrc>{GST_APP_SRC(source.get())};
  const auto app_sink = GstView<GstAppSink>{GST_APP_SINK(preview_sink.get())};

  {
    auto caps = CapsPtr{gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
        kWidth, "height", G_TYPE_INT, kHeight, "framerate", GST_TYPE_FRACTION,
        kFramesPerSecond, 1, nullptr)};
    gst_app_src_set_caps(app_src, caps.get());
  }

  std::atomic_uint main_frames = 0;
  g_object_set(main_sink.get(), "signal-handoffs", TRUE, nullptr);
  g_signal_connect(main_sink.get(), "handoff", G_CALLBACK(OnHandoff),
                   &main_frames);

  std::atomic_uint preview_frames = 0;
  std::jthread preview_drain([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      SamplePtr sample{
          gst_app_sink_try_pull_sample(app_sink, 50 * GST_MSECOND)};
      if (sample != nullptr) {
        ++preview_frames;
      }
    }
  });

  // Put the pipeline on the shared timeline exactly like StartOutput
  // does: pinned system clock, application-owned start/base time, and
  // automatic latency (#437).
  ClockPtr clock{gst_system_clock_obtain()};
  gst_pipeline_use_clock(GST_PIPELINE(pipeline.get()), clock.get());
  gst_element_set_start_time(pipeline.get(), GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(pipeline.get(), gst_clock_get_time(clock.get()));

  REQUIRE(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) !=
          GST_STATE_CHANGE_FAILURE);

  const auto running_time = [&] {
    const auto now = gst_clock_get_time(clock.get());
    const auto base = gst_element_get_base_time(pipeline.get());
    return now > base ? now - base : 0;
  };

  const auto push = [&](int count) {
    // 10 frames of head start so nothing is late at the synced sink.
    std::jthread pusher([&] {
      CHECK(PushFrames(app_src, count,
                       running_time() + 10 * kFrameDuration) == count);
    });
    pusher.join();
    // Let the tail frames drain through the synced sink.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
  };

  push(60);

  INFO("main branch rendered ", main_frames.load(), " of 60 pushed frames");
  CHECK(main_frames.load() > 50);
  CHECK(preview_frames.load() == 0);

  const auto main_before = main_frames.load();
  gate.active.store(true);

  // A gap with no preview input, like a closed-gate period. The gate
  // decimates rather than rate-converts, so there is no cadence to go
  // stale: exactly every 6th of the 60 pushed frames may pass.
  std::this_thread::sleep_for(std::chrono::seconds{2});

  push(60);

  CHECK(main_frames.load() > main_before + 50);
  CHECK(preview_frames.load() > 0);
  INFO("preview produced ", preview_frames.load(), " frames for ~1 s");
  CHECK(preview_frames.load() <= 15);

  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
}
