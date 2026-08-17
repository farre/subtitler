#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#include "stream/stream.h"

namespace {

volatile std::sig_atomic_t signal_received = 0;

extern "C" void HandleSignal(int) { signal_received = 1; }

std::optional<int> ParseInteger(std::string_view text) {
  int value{};

  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);

  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }

  return value;
}

}  // namespace

int main(int argc, char** argv) {
  std::string device = "/dev/video0";
  subtitler::OutputMode output_mode = subtitler::OutputMode::kKmsSoftware;
  std::optional<int> connector_id;
  bool audio = true;
  std::optional<std::string> audio_output_device;
  int positional = 0;

  const auto usage = [&] {
    std::println(stderr,
                 "Usage: {} [video-device] [connector-id] "
                 "[--output=software|pisp|window|null] [--no-audio] "
                 "[--audio-output-device=<alsa-device>]",
                 argv[0]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};

    if (arg == "-h" || arg == "--help") {
      usage();
      return EXIT_SUCCESS;
    } else if (arg == "--output=pisp") {
      output_mode = subtitler::OutputMode::kKmsPisp;
    } else if (arg == "--output=software") {
      output_mode = subtitler::OutputMode::kKmsSoftware;
    } else if (arg == "--output=window") {
      output_mode = subtitler::OutputMode::kWindow;
    } else if (arg == "--output=null") {
      output_mode = subtitler::OutputMode::kNull;
    } else if (arg == "--no-audio") {
      audio = false;
    } else if (arg.starts_with("--audio-output-device=")) {
      audio_output_device = std::string{
          arg.substr(std::string_view{"--audio-output-device="}.size())};
    } else if (arg.starts_with("--")) {
      std::println(stderr, "Unknown option: {}", arg);
      usage();
      return EXIT_FAILURE;
    } else if (positional == 0) {
      device = arg;
      ++positional;
    } else if (positional == 1) {
      connector_id = ParseInteger(arg);

      if (!connector_id) {
        std::println(stderr, "Invalid DRM connector ID: {}", arg);
        return EXIT_FAILURE;
      }

      ++positional;
    } else {
      usage();
      return EXIT_FAILURE;
    }
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  auto stream = subtitler::Stream::Create(device, output_mode, connector_id,
                                          audio, audio_output_device);

  if (!stream) {
    std::println(stderr, "Failed to create stream");
    return EXIT_FAILURE;
  }

  while (!signal_received) {
    stream->Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  stream->Stop();

  std::println("Stopped. Application buffer dropped {} frames.",
               stream->DroppedFrames());

  return stream->Failed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
