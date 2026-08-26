#include <doctest/doctest.h>
#include <gst/gst.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "stream/deleters.h"
#include "stream/stream.h"

TEST_CASE("stream throughput with audio enabled") {
  // The ALSA null PCM playback device emulates real hardware timing
  // (latency reporting, clock provision) without a sound card, so this
  // exercises the output pipeline's clock/latency negotiation
  // headlessly. Capture must come from a real CV105 (the ALSA null
  // capture plugin is not rate-limited and would poison the capture
  // pipeline's clock). Override the video device with
  // SUBTITLER_TEST_VIDEO_DEVICE.
  const char* env_device = std::getenv("SUBTITLER_TEST_VIDEO_DEVICE");
  const std::string video_device =
      env_device != nullptr ? env_device : "/dev/video0";

  auto stream = subtitler::Stream::Create(
      video_device, subtitler::OutputMode::kNull, std::nullopt, /*audio=*/true,
      /*audio_output_device=*/"null");

  if (!stream) {
    MESSAGE("skipping: no usable CV105 capture device at ", video_device);
    return;
  }

  // The pipeline latency is reconfigured when the audio sink starts
  // (GST_MESSAGE_LATENCY -> gst_bin_recalculate_latency), a few seconds
  // in; run well past that point.
  for (int i = 0; i < 16; ++i) {
    stream->Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  stream->Stop();

  CHECK(stream->DroppedFrames() == 0);
  CHECK_FALSE(stream->Failed());
}

TEST_CASE("whisper tap flows converted audio") {
  gst_init(nullptr, nullptr);

  const subtitler::GstPointer<GstElementFactory> source_factory{
      gst_element_factory_find("audiotestsrc")};
  if (source_factory == nullptr) {
    MESSAGE("skipping: no audiotestsrc");
    return;
  }

  // audiotestsrc stands in for the CV105's alsasrc: no capture hardware
  // on a dev machine. Only the audio half of the capture pipeline runs;
  // the passthrough appsink is left undrained on purpose — its dropping
  // queue is what lets the tee flow regardless.
  auto description =
      subtitler::AudioCapturePipelineDescription("hw:CARD=Video,DEV=0", true) +
      " " + subtitler::WhisperCapturePipelineDescription();
  const std::string alsasrc =
      "alsasrc device=\"hw:CARD=Video,DEV=0\" do-timestamp=true";
  const auto at = description.find(alsasrc);
  REQUIRE(at != std::string::npos);
  description.replace(at, alsasrc.size(),
                      "audiotestsrc is-live=true do-timestamp=true");

  subtitler::GstPointer<GError> error;
  const subtitler::GstPointer<GstElement> pipeline{
      gst_parse_launch(description.c_str(), std::out_ptr(error))};
  INFO("gst_parse_launch error: ",
       error != nullptr ? std::string{error->message} : "none");
  REQUIRE(error == nullptr);

  const subtitler::GstPointer<GstElement> sink{
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "whisper_sink")};
  REQUIRE(sink != nullptr);

  // The timeline configuration the app applies (ConfigureTimeline):
  // pinned system clock, application-owned start/base time.
  subtitler::ClockPtr clock{gst_system_clock_obtain()};
  gst_pipeline_use_clock(GST_PIPELINE(pipeline.get()), clock.get());
  gst_element_set_start_time(pipeline.get(), GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(pipeline.get(), gst_clock_get_time(clock.get()));

  REQUIRE(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) !=
          GST_STATE_CHANGE_FAILURE);

  // Drain the tap for a couple of seconds: the branch must flow, in the
  // whisper format (16 kHz mono F32LE = 64000 bytes/s). The source is
  // live, so the audio accumulates in real time — loop on a wall-clock
  // deadline, not a pull count.
  auto* app_sink = GST_APP_SINK(sink.get());
  std::uint64_t bytes = 0;
  bool caps_checked = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{15};

  while (bytes < 2 * 64000 && std::chrono::steady_clock::now() < deadline) {
    const subtitler::GstPointer<GstSample> sample{
        gst_app_sink_try_pull_sample(app_sink, GST_SECOND)};
    if (sample == nullptr) {
      continue;
    }

    if (!caps_checked) {
      caps_checked = true;
      // gst_sample_get_caps is transfer-none.
      const GstCaps* caps = gst_sample_get_caps(sample.get());
      REQUIRE(caps != nullptr);
      const GstStructure* structure = gst_caps_get_structure(caps, 0);
      int rate = 0;
      int channels = 0;
      CHECK(std::string{gst_structure_get_name(structure)} == "audio/x-raw");
      CHECK(std::string{gst_structure_get_string(structure, "format")} ==
            "F32LE");
      CHECK(gst_structure_get_int(structure, "rate", &rate));
      CHECK(rate == 16000);
      CHECK(gst_structure_get_int(structure, "channels", &channels));
      CHECK(channels == 1);
    }

    bytes += gst_buffer_get_size(gst_sample_get_buffer(sample.get()));
  }

  gst_element_set_state(pipeline.get(), GST_STATE_NULL);

  INFO("flowed ", bytes, " of ", 2 * 64000, " expected bytes");
  CHECK(bytes >= 2 * 64000);
}
