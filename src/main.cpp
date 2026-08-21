#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "stream/stream.h"
#include "utils/logging.h"
#include "utils/paths.h"
#include "web/web_server.h"

namespace {

constexpr std::uint16_t kWebPort = 8080;

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
  int audio_offset_ms = 0;
  bool web = false;
  std::optional<std::filesystem::path> web_root;
  std::optional<std::string> subtitles;
  // The active library title, mirrored for the web state endpoint.
  // Written at boot resume and, once the server runs, only on its io
  // thread.
  std::optional<std::string> active_title;
  int positional = 0;

  const auto usage = [&] {
    std::println(stderr,
                 "Usage: {} [video-device] [connector-id] "
                 "[--output=software|pisp|window|null] [--no-audio] "
                 "[--audio-output-device=<alsa-device>] "
                 "[--audio-offset=<ms>] [--subtitles=<srt-file>] "
                 "[--web] [--web-root=<dir>]",
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
    } else if (arg == "--web") {
      web = true;
    } else if (arg.starts_with("--audio-output-device=")) {
      audio_output_device = std::string{
          arg.substr(std::string_view{"--audio-output-device="}.size())};
    } else if (arg.starts_with("--audio-offset=")) {
      const auto offset =
          ParseInteger(arg.substr(std::string_view{"--audio-offset="}.size()));
      if (!offset) {
        std::println(stderr, "Invalid audio offset: {}", arg);
        return EXIT_FAILURE;
      }
      audio_offset_ms = *offset;
    } else if (arg.starts_with("--subtitles=")) {
      subtitles =
          std::string{arg.substr(std::string_view{"--subtitles="}.size())};
    } else if (arg.starts_with("--web-root=")) {
      web_root =
          std::string{arg.substr(std::string_view{"--web-root="}.size())};
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

  const auto state_dir = subtitler::StateDirectory();

  if (subtitles) {
    // An explicit flag must name a real file: a missing one would only
    // surface later as a filesrc bus error.
    if (!std::filesystem::exists(*subtitles)) {
      std::println(stderr, "Subtitle file not found: {}", *subtitles);
      return EXIT_FAILURE;
    }
  } else if (state_dir) {
    // Boot resume: replay the SRT selected the last time around (#438).
    if (const auto active = subtitler::ActiveSubtitleFile(*state_dir)) {
      subtitles = active->string();
      active_title = active->filename().string();
      MAIN_LOG(subtitler::LogLevel::kInfo, "Resuming subtitles from {}",
               *subtitles);
    }
  }

  auto stream = subtitler::Stream::Create(device, output_mode, connector_id,
                                          audio, audio_output_device,
                                          audio_offset_ms, web, subtitles);

  if (!stream) {
    std::println(stderr, "Failed to create stream");
    return EXIT_FAILURE;
  }

  // Declared after stream, so it is destroyed first: the server reads from
  // the stream-owned preview frame buffer and flips its preview gate.
  std::unique_ptr<subtitler::WebServer> web_server;

  if (web) {
    if (!web_root) {
      web_root = subtitler::WebRootDirectory();
    }

    subtitler::WebServerHooks hooks;
    hooks.preview_activation = [&stream](bool active) {
      stream->SetPreviewActive(active);
    };
    hooks.web_root = web_root;

    // Uploads (#212) and the library/state endpoints (#441): store into
    // the state-dir library, mark active for boot resume, and
    // live-switch the stream to the new SRT.
    if (state_dir) {
      hooks.subtitle_upload =
          [&stream, &state_dir, &active_title](
              std::string_view title,
              std::string_view contents) -> subtitler::SubtitleUploadResult {
        const auto relative = subtitler::LibrarySubtitlePath(title);
        if (!relative) {
          return {subtitler::SubtitleUploadStatus::kInvalidTitle, {}};
        }

        const auto stored =
            subtitler::StoreSubtitle(*state_dir, title, contents);
        if (!stored || !stream->SetSubtitleFile(stored->string())) {
          return {subtitler::SubtitleUploadStatus::kFailed, {}};
        }

        active_title = std::string{title};
        return {subtitler::SubtitleUploadStatus::kStored,
                relative->generic_string()};
      };

      hooks.subtitle_list = [&state_dir] {
        return subtitler::ListSubtitles(*state_dir);
      };

      hooks.subtitle_state_get = [&stream, &active_title] {
        return subtitler::SubtitleState{
            .file = active_title,
            .visible = stream->SubtitlesVisible(),
            .paused = stream->SubtitlesPaused(),
            .time_ms = stream->SubtitleTime().value_or(0),
            .delay_ms = stream->SubtitleDelay(),
        };
      };

      hooks.subtitle_state_set =
          [&stream, &state_dir, &active_title](
              const subtitler::SubtitleStatePatch& patch) -> bool {
        if (patch.file) {
          if (patch.file->empty()) {
            if (!stream->SetSubtitleFile(std::nullopt)) {
              return false;
            }
            subtitler::ClearActiveSubtitle(*state_dir);
            active_title = std::nullopt;
          } else {
            const auto relative =
                subtitler::FindLibrarySubtitle(*state_dir, *patch.file);
            if (!relative || !stream->SetSubtitleFile(
                                 (*state_dir / "subtitles" / *relative)
                                     .string())) {
              return false;
            }
            if (!subtitler::SetActiveSubtitle(*state_dir,
                                              relative->generic_string())) {
              MAIN_LOG(subtitler::LogLevel::kWarning,
                       "Could not write the active subtitle marker");
            }
            active_title = *patch.file;
          }
        }

        // After a file switch: it restarts at position 0 with the delay
        // reset, so the remaining changes apply afterwards.
        if (patch.visible) {
          stream->SetSubtitlesVisible(*patch.visible);
        }
        if (patch.paused) {
          stream->SetSubtitlesPaused(*patch.paused);
        }
        if (patch.time_ms) {
          stream->SetSubtitleTime(*patch.time_ms);
        }
        if (patch.delay_ms) {
          stream->SetSubtitleDelay(*patch.delay_ms);
        }

        return true;
      };
    }

    web_server = subtitler::WebServer::Create(
        kWebPort, stream->PreviewFrames(), std::move(hooks));

    if (!web_server) {
      std::println(stderr, "Failed to start the web server");
      return EXIT_FAILURE;
    }

    if (web_root) {
      MAIN_LOG(subtitler::LogLevel::kInfo,
               "Web interface available on port {}, serving {}", kWebPort,
               web_root->string());
    } else {
      MAIN_LOG(subtitler::LogLevel::kInfo,
               "Web interface available on port {} (no web root found; static "
               "files disabled)",
               kWebPort);
    }
  }

  while (!signal_received) {
    stream->Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  stream->Stop();

  MAIN_LOG(subtitler::LogLevel::kInfo,
           "Stopped. Application buffer dropped {} frames.",
           stream->DroppedFrames());

  return stream->Failed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
