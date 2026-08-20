#include <doctest/doctest.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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

// Pushes count solid-fill YUY2 frames (every byte kSolidFill), so the
// handoff can spot subtitle compositing as pixels that deviate from the
// fill.
constexpr std::uint8_t kSolidFill = 0x10;

bool PushSolidFrame(GstView<GstAppSrc> source, std::uint64_t pts) {
  BufferPtr buffer{
      gst_buffer_new_allocate(nullptr, kWidth * kHeight * 2, nullptr)};

  if (buffer == nullptr) {
    return false;
  }

  gst_buffer_memset(buffer.get(), 0, kSolidFill, kWidth * kHeight * 2);
  GST_BUFFER_PTS(buffer.get()) = pts;
  GST_BUFFER_DURATION(buffer.get()) = kFrameDuration;

  return gst_app_src_push_buffer(source, buffer.release()) == GST_FLOW_OK;
}

int PushSolidFrames(GstView<GstAppSrc> source, int count,
                    std::uint64_t first_pts) {
  for (int i = 0; i < count; ++i) {
    if (!PushSolidFrame(source, first_pts + i * kFrameDuration)) {
      return i;
    }
  }

  return count;
}

struct SubtitleRenderStats {
  std::atomic_uint frames = 0;
  std::atomic_uint modified = 0;
  std::atomic_uint64_t first_modified = GST_CLOCK_TIME_NONE;
  std::atomic_uint64_t last_modified = 0;
};

void OnSubtitleHandoff(GstElement*, GstBuffer* buffer, GstPad*,
                       gpointer user_data) {
  auto& stats = *static_cast<SubtitleRenderStats*>(user_data);

  ++stats.frames;

  GstMapInfo info;
  if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
    return;
  }

  std::size_t differing = 0;
  for (gsize i = 0; i < info.size; ++i) {
    if (info.data[i] != kSolidFill) {
      ++differing;
    }
  }

  gst_buffer_unmap(buffer, &info);

  if (differing > info.size / 1000) {
    ++stats.modified;

    const auto pts = GST_BUFFER_PTS(buffer);
    auto first = stats.first_modified.load();
    while (pts < first &&
           !stats.first_modified.compare_exchange_weak(first, pts)) {
    }
    auto last = stats.last_modified.load();
    while (pts > last &&
           !stats.last_modified.compare_exchange_weak(last, pts)) {
    }
  }
}

// The real output pipeline with a subtitle branch, wired for pixel
// checking: the sink's handoff counts frames deviating from the solid
// fill. The SRT is anchored via the parser pad offset, the same call
// StartOutput uses. The destructor stops the pipeline and removes the
// SRT file.
struct SubtitleRig {
  SubtitleRig() = default;
  SubtitleRig(SubtitleRig&&) = default;
  SubtitleRig& operator=(SubtitleRig&&) = default;
  ~SubtitleRig() {
    if (pipeline != nullptr) {
      gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    }
    std::error_code error;
    std::filesystem::remove(srt_path, error);
  }

  ElementPtr pipeline;
  ElementPtr source;
  PadPtr parser_src;
  std::filesystem::path srt_path;
};

std::optional<SubtitleRig> StartSubtitlePipeline(std::string_view srt,
                                                 gint64 anchor,
                                                 SubtitleRenderStats& stats) {
  SubtitleRig rig;
  rig.srt_path =
      std::filesystem::temp_directory_path() / "subtitler-test.srt";
  {
    std::ofstream file{rig.srt_path};
    file << srt;
  }

  const std::string description = subtitler::OutputPipelineDescription(
      subtitler::OutputMode::kNull, std::nullopt, std::nullopt, false,
      rig.srt_path.string());

  ErrorPtr error;
  rig.pipeline = ElementPtr{gst_parse_launch(description.c_str(),
                                             std::out_ptr(error))};
  INFO("gst_parse_launch error: ",
       error != nullptr ? std::string{error->message} : "none");
  CHECK(error == nullptr);
  if (rig.pipeline == nullptr) {
    return std::nullopt;
  }

  rig.source = ElementPtr{
      gst_bin_get_by_name(GST_BIN(rig.pipeline.get()), "output_source")};
  ElementPtr parser{
      gst_bin_get_by_name(GST_BIN(rig.pipeline.get()), "subtitle_parser")};
  CHECK(rig.source != nullptr);
  CHECK(parser != nullptr);
  if (rig.source == nullptr || parser == nullptr) {
    return std::nullopt;
  }

  rig.parser_src = PadPtr{gst_element_get_static_pad(parser.get(), "src")};
  CHECK(rig.parser_src != nullptr);
  if (rig.parser_src == nullptr) {
    return std::nullopt;
  }
  gst_pad_set_offset(rig.parser_src.get(), anchor);

  ElementPtr main_sink = FindMainSink(rig.pipeline.get(), nullptr);
  CHECK(main_sink != nullptr);
  if (main_sink == nullptr) {
    return std::nullopt;
  }

  const auto app_src = GstView<GstAppSrc>{GST_APP_SRC(rig.source.get())};
  auto caps = CapsPtr{gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
      kWidth, "height", G_TYPE_INT, kHeight, "framerate", GST_TYPE_FRACTION,
      kFramesPerSecond, 1, nullptr)};
  gst_app_src_set_caps(app_src, caps.get());

  g_object_set(main_sink.get(), "signal-handoffs", TRUE, nullptr);
  g_signal_connect(main_sink.get(), "handoff", G_CALLBACK(OnSubtitleHandoff),
                   &stats);

  ClockPtr clock{gst_system_clock_obtain()};
  gst_pipeline_use_clock(GST_PIPELINE(rig.pipeline.get()), clock.get());
  gst_element_set_start_time(rig.pipeline.get(), GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(rig.pipeline.get(),
                            gst_clock_get_time(clock.get()));

  CHECK(gst_element_set_state(rig.pipeline.get(), GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

  return rig;
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
  ElementPtr pipeline{
      gst_parse_launch(description.c_str(), std::out_ptr(error))};
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
      CHECK(PushFrames(app_src, count, running_time() + 10 * kFrameDuration) ==
            count);
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

// Subtitle anchoring (#438): an SRT cue must land exactly on the frames
// whose running time is anchor + cue time, and stay invisible outside the
// cue window. Exercises the real description with the production
// mechanism: a pad offset on the named subparse element's src pad, the
// same call StartOutput uses (positive offsets delay, so the anchor is a
// positive offset).
// One cue, 0.2 s to 1.4 s of SRT time; shared by the subtitle tests.
constexpr std::string_view kOneCueSrt =
    "1\n00:00:00,200 --> 00:00:01,400\nHello subtitles\n\n";
constexpr gint64 kOneCueAnchor = 1 * GST_SECOND;
constexpr GstClockTime kOneCueStart = kOneCueAnchor + 200 * GST_MSECOND;
constexpr GstClockTime kOneCueEnd = kOneCueAnchor + 1400 * GST_MSECOND;
constexpr auto kEdgeTolerance = 2 * kFrameDuration;

bool SubtitleElementsAvailable() {
  subtitler::GstPointer<GstElementFactory> overlay_factory{
      gst_element_factory_find("subtitleoverlay")};
  subtitler::GstPointer<GstElementFactory> subparse_factory{
      gst_element_factory_find("subparse")};
  return overlay_factory != nullptr && subparse_factory != nullptr;
}

TEST_CASE("output pipeline subtitle branch") {
  gst_init(nullptr, nullptr);

  if (!SubtitleElementsAvailable()) {
    return;
  }

  SubtitleRenderStats stats;
  auto rig = StartSubtitlePipeline(kOneCueSrt, kOneCueAnchor, stats);
  REQUIRE(rig.has_value());

  const auto app_src = GstView<GstAppSrc>{GST_APP_SRC(rig->source.get())};

  // 3.5 s of frames starting at running time 0: the cue window [1.2 s,
  // 2.4 s) sits comfortably inside, with unmodified frames on both sides.
  constexpr int kFrameCount = 210;
  {
    std::jthread pusher([&] {
      CHECK(PushSolidFrames(app_src, kFrameCount, 0) == kFrameCount);
    });
    pusher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  INFO("rendered ", stats.modified.load(), " subtitled frames of ",
       stats.frames.load());
  CHECK(stats.frames.load() == kFrameCount);

  // Text actually rendered (needs fonts; the Pi target installs them).
  REQUIRE(stats.modified.load() > 0);

  // Expected coverage: 1.2 s at 60 fps = 72 frames, with slack for
  // boundary semantics at the cue edges.
  CHECK(stats.modified.load() >= 60);
  CHECK(stats.modified.load() <= 84);
  CHECK(stats.first_modified.load() >= kOneCueStart - kEdgeTolerance);
  CHECK(stats.first_modified.load() < kOneCueStart + 4 * kFrameDuration);
  CHECK(stats.last_modified.load() >= kOneCueEnd - 4 * kFrameDuration);
  CHECK(stats.last_modified.load() < kOneCueEnd + kEdgeTolerance);
}

// SetSubtitleTime and the delay trim re-emit the SRT through a new pad
// offset (#439): a pad offset reaches only cues parsed after the change,
// so the branch is flush-seeked back to the start and subparse
// re-parses. The cue partially rendered in phase 1 must render again at
// its moved window — and only there, proving the flush cleared the
// overlay's old queue.
TEST_CASE("output pipeline subtitle reparse") {
  gst_init(nullptr, nullptr);

  if (!SubtitleElementsAvailable()) {
    return;
  }

  SubtitleRenderStats stats;
  auto rig = StartSubtitlePipeline(kOneCueSrt, kOneCueAnchor, stats);
  REQUIRE(rig.has_value());

  ElementPtr subtitle_source{gst_bin_get_by_name(GST_BIN(rig->pipeline.get()),
                                                 "subtitle_source")};
  REQUIRE(subtitle_source != nullptr);

  const auto app_src = GstView<GstAppSrc>{GST_APP_SRC(rig->source.get())};

  // Phase 1: 1.5 s of frames; the cue renders [1.2 s, 1.5 s) of its
  // [1.2 s, 2.4 s) window.
  {
    std::jthread pusher([&] {
      CHECK(PushSolidFrames(app_src, 90, 0) == 90);
    });
    pusher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  constexpr GstClockTime kPhase1Last = 89 * kFrameDuration;

  INFO("phase 1 rendered ", stats.modified.load(), " of ",
       stats.frames.load());
  CHECK(stats.modified.load() >= 12);
  CHECK(stats.modified.load() <= 24);
  CHECK(stats.first_modified.load() >= kOneCueStart - kEdgeTolerance);
  CHECK(stats.first_modified.load() < kOneCueStart + 4 * kFrameDuration);
  CHECK(stats.last_modified.load() >= kPhase1Last - 4 * kFrameDuration);
  CHECK(stats.last_modified.load() < kPhase1Last + kEdgeTolerance);

  // Phase 2: re-anchor at 2.5 s and re-emit: the cue renders at
  // [2.7 s, 3.9 s).
  stats.frames.store(0);
  stats.modified.store(0);
  stats.first_modified.store(GST_CLOCK_TIME_NONE);
  stats.last_modified.store(0);
  gst_pad_set_offset(rig->parser_src.get(), 2500 * GST_MSECOND);
  CHECK(gst_element_seek_simple(subtitle_source.get(), GST_FORMAT_BYTES,
                                GST_SEEK_FLAG_FLUSH, 0));

  {
    std::jthread pusher([&] {
      CHECK(PushSolidFrames(app_src, 180, 90 * kFrameDuration) == 180);
    });
    pusher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  constexpr GstClockTime kMovedStart = 2700 * GST_MSECOND;
  constexpr GstClockTime kMovedEnd = 3900 * GST_MSECOND;

  INFO("phase 2 rendered ", stats.modified.load(), " of ",
       stats.frames.load());
  CHECK(stats.frames.load() == 180);
  REQUIRE(stats.modified.load() > 0);
  CHECK(stats.modified.load() >= 60);
  CHECK(stats.modified.load() <= 84);
  CHECK(stats.first_modified.load() >= kMovedStart - kEdgeTolerance);
  CHECK(stats.first_modified.load() < kMovedStart + 4 * kFrameDuration);
  CHECK(stats.last_modified.load() >= kMovedEnd - 4 * kFrameDuration);
  CHECK(stats.last_modified.load() < kMovedEnd + kEdgeTolerance);
}

// Pause hides the subtitles and freezes the SRT position (#439); resume
// re-anchors at the frozen position and re-emits. The hidden stretch
// must render nothing, and after resume the cue plays out from the
// frozen position.
TEST_CASE("output pipeline subtitle pause and resume") {
  gst_init(nullptr, nullptr);

  if (!SubtitleElementsAvailable()) {
    return;
  }

  SubtitleRenderStats stats;
  auto rig = StartSubtitlePipeline(kOneCueSrt, kOneCueAnchor, stats);
  REQUIRE(rig.has_value());

  ElementPtr overlay{gst_bin_get_by_name(GST_BIN(rig->pipeline.get()),
                                         "subtitle_overlay")};
  ElementPtr subtitle_source{gst_bin_get_by_name(GST_BIN(rig->pipeline.get()),
                                                 "subtitle_source")};
  REQUIRE(overlay != nullptr);
  REQUIRE(subtitle_source != nullptr);

  const auto app_src = GstView<GstAppSrc>{GST_APP_SRC(rig->source.get())};

  // Phase 1: the cue starts rendering at 1.2 s.
  {
    std::jthread pusher([&] {
      CHECK(PushSolidFrames(app_src, 90, 0) == 90);
    });
    pusher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  INFO("phase 1 rendered ", stats.modified.load(), " of ",
       stats.frames.load());
  CHECK(stats.modified.load() >= 12);

  // Pause at position 0.5 s (running time 1.5 s, anchor 1 s): hidden
  // while paused, even though the cue's window is still open.
  stats.frames.store(0);
  stats.modified.store(0);
  g_object_set(overlay.get(), "silent", TRUE, nullptr);

  {
    std::jthread pusher([&] {
      CHECK(PushSolidFrames(app_src, 60, 90 * kFrameDuration) == 60);
    });
    pusher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  INFO("paused rendered ", stats.modified.load(), " of ",
       stats.frames.load());
  CHECK(stats.frames.load() == 60);
  CHECK(stats.modified.load() == 0);

  // Resume: re-anchor so the frozen position 0.5 s lands at running time
  // 2.5 s and re-emit; the cue plays out over [2.5 s, 3.4 s).
  stats.frames.store(0);
  stats.modified.store(0);
  stats.first_modified.store(GST_CLOCK_TIME_NONE);
  stats.last_modified.store(0);
  gst_pad_set_offset(rig->parser_src.get(), 2000 * GST_MSECOND);
  CHECK(gst_element_seek_simple(subtitle_source.get(), GST_FORMAT_BYTES,
                                GST_SEEK_FLAG_FLUSH, 0));
  g_object_set(overlay.get(), "silent", FALSE, nullptr);

  {
    std::jthread pusher([&] {
      CHECK(PushSolidFrames(app_src, 120, 150 * kFrameDuration) == 120);
    });
    pusher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  constexpr GstClockTime kResumedStart = 2500 * GST_MSECOND;
  constexpr GstClockTime kResumedEnd = 3400 * GST_MSECOND;

  INFO("resumed rendered ", stats.modified.load(), " of ",
       stats.frames.load());
  CHECK(stats.frames.load() == 120);
  REQUIRE(stats.modified.load() > 0);
  CHECK(stats.modified.load() >= 46);
  CHECK(stats.modified.load() <= 64);
  CHECK(stats.first_modified.load() >= kResumedStart - kEdgeTolerance);
  CHECK(stats.first_modified.load() < kResumedStart + 4 * kFrameDuration);
  CHECK(stats.last_modified.load() >= kResumedEnd - 4 * kFrameDuration);
  CHECK(stats.last_modified.load() < kResumedEnd + kEdgeTolerance);
}
