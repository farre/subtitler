#include <doctest/doctest.h>
#include <gst/gst.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

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
