#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "config/config.h"
#include "stream/fonts.h"
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

std::optional<subtitler::OutputMode> ParseOutputMode(std::string_view name) {
  if (name == "software") {
    return subtitler::OutputMode::kKmsSoftware;
  }
  if (name == "pisp") {
    return subtitler::OutputMode::kKmsPisp;
  }
  if (name == "window") {
    return subtitler::OutputMode::kWindow;
  }
  if (name == "null") {
    return subtitler::OutputMode::kNull;
  }
  return std::nullopt;
}

// The persisted style, (re)applied whenever subtitles attach: a file
// switch resets the delay, and the setters are no-ops without subtitles,
// so values configured while detached land here on the next attach.
void ApplySubtitleStyle(subtitler::Stream& stream,
                        const subtitler::Config& config) {
  const auto& values = config.values();
  if (values.subtitle_font_family) {
    stream.SetSubtitleFontFamily(*values.subtitle_font_family);
  }
  if (values.subtitle_font_size_pt) {
    stream.SetSubtitleFontSize(*values.subtitle_font_size_pt);
  }
  if (values.subtitle_font_color) {
    stream.SetSubtitleFontColor(*values.subtitle_font_color);
  }
  if (values.subtitle_delay_ms) {
    stream.SetSubtitleDelay(*values.subtitle_delay_ms);
  }
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
  std::optional<std::string> api_key;
  std::optional<std::string> subtitles;
  // A --subtitles flag names a file strictly; a config-named file that's
  // gone is dropped like a dangling boot-resume marker (#220).
  bool explicit_subtitles = false;
  // The active library title, mirrored for the web state endpoint.
  // Written at boot resume and, once the server runs, only on its io
  // thread.
  std::optional<std::string> active_title;
  // The whisper tap's ggml model (#19 spike); experimental.
  std::optional<std::string> whisper_model;
  int positional = 0;
  // Values explicitly supplied on the command line persist to the
  // config after a successful parse.
  bool persist_device = false;
  bool persist_web = false;
  bool persist_web_root = false;
  bool persist_api_key = false;

  const auto usage = [&] {
    std::println(stderr,
                 "Usage: {} [video-device] [connector-id] "
                 "[--config=<path>] "
                 "[--output=software|pisp|window|null] [--no-audio] "
                 "[--audio-output-device=<alsa-device>] "
                 "[--audio-offset=<ms>] [--subtitles=<srt-file>] "
                 "[--web] [--web-root=<dir>] [--api-key=<key>] "
                 "[--whisper=<ggml-model>]",
                 argv[0]);
  };

  // The config file (#16): --config=<path>, else
  // <ConfigDirectory()>/config.ini. A missing file starts empty and
  // comes into existence on the first web-API write-back (#221).
  std::optional<std::filesystem::path> config_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg.starts_with("--config=")) {
      const auto value = arg.substr(std::string_view{"--config="}.size());
      if (value.empty()) {
        std::println(stderr, "Empty --config path");
        return EXIT_FAILURE;
      }
      config_path = std::string{value};
    }
  }
  if (!config_path) {
    if (const auto dir = subtitler::ConfigDirectory()) {
      config_path = *dir / "config.ini";
    }
  }

  std::unique_ptr<subtitler::Config> config;
  if (config_path) {
    config = subtitler::Config::Load(*config_path);
  }

  // The file provides the base values; the command line overrides them.
  if (config) {
    const auto& values = config->values();
    if (values.device) {
      device = *values.device;
    }
    if (values.audio) {
      audio = *values.audio;
    }
    if (values.output_mode) {
      if (const auto mode = ParseOutputMode(*values.output_mode)) {
        output_mode = *mode;
      } else {
        std::println(stderr, "Ignoring invalid config output mode: {}",
                     *values.output_mode);
      }
    }
    if (values.connector_id) {
      connector_id = *values.connector_id;
    }
    if (values.audio_output_device) {
      audio_output_device = *values.audio_output_device;
    }
    if (values.audio_offset_ms) {
      audio_offset_ms = *values.audio_offset_ms;
    }
    if (values.web) {
      web = *values.web;
    }
    if (values.web_root) {
      web_root = *values.web_root;
    }
    if (values.api_key) {
      api_key = *values.api_key;
    }
    if (values.subtitle_file) {
      subtitles = *values.subtitle_file;
    }
  }

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};

    if (arg == "-h" || arg == "--help") {
      usage();
      return EXIT_SUCCESS;
    } else if (arg.starts_with("--output=")) {
      const auto mode =
          ParseOutputMode(arg.substr(std::string_view{"--output="}.size()));
      if (!mode) {
        std::println(stderr, "Invalid output mode: {}", arg);
        usage();
        return EXIT_FAILURE;
      }
      output_mode = *mode;
    } else if (arg.starts_with("--config=")) {
      // Already consumed by the pre-scan.
    } else if (arg == "--no-audio") {
      audio = false;
    } else if (arg == "--web") {
      web = true;
      persist_web = true;
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
      explicit_subtitles = true;
    } else if (arg.starts_with("--web-root=")) {
      web_root =
          std::string{arg.substr(std::string_view{"--web-root="}.size())};
      persist_web_root = true;
    } else if (arg.starts_with(std::string_view("--api-key="))) {
      api_key = std::string{arg.substr(std::string_view{"--api-key="}.size())};
      persist_api_key = true;
    } else if (arg.starts_with("--whisper=")) {
      whisper_model =
          std::string{arg.substr(std::string_view{"--whisper="}.size())};
    } else if (arg.starts_with("--")) {
      std::println(stderr, "Unknown option: {}", arg);
      usage();
      return EXIT_FAILURE;
    } else if (positional == 0) {
      device = arg;
      persist_device = true;
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

  // Flags explicitly given on the command line persist: the next run
  // picks them up from the file without repeating them.
  if (config &&
      (persist_device || persist_web || persist_web_root || persist_api_key)) {
    if (persist_device) {
      config->SetDevice(device);
    }
    if (persist_web) {
      config->SetWebEnabled(web);
    }
    if (persist_web_root) {
      config->SetWebRoot(web_root->string());
    }
    if (persist_api_key) {
      config->SetApiKey(*api_key);
    }
    if (!config->Save()) {
      MAIN_LOG(subtitler::LogLevel::kWarning,
               "Could not save the configuration");
    }
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  const auto state_dir = subtitler::StateDirectory();

  if (subtitles && !std::filesystem::exists(*subtitles)) {
    // An explicit flag must name a real file: a missing one would only
    // surface later as a filesrc bus error.
    if (explicit_subtitles) {
      std::println(stderr, "Subtitle file not found: {}", *subtitles);
      return EXIT_FAILURE;
    }
    std::println(stderr, "Configured subtitle file not found: {}", *subtitles);
    subtitles = std::nullopt;
  }

  if (!subtitles && state_dir) {
    // Boot resume: replay the SRT selected the last time around (#438).
    if (const auto active = subtitler::ActiveSubtitleFile(*state_dir)) {
      subtitles = active->string();
      active_title = active->filename().string();
      MAIN_LOG(subtitler::LogLevel::kInfo, "Resuming subtitles from {}",
               *subtitles);
    }
  }

  // A configured or explicit path that names a library entry reports as
  // that title, so the state endpoint and the web UI can select it;
  // anything else (--subtitles outside the library) stays untitled.
  if (subtitles && !active_title && state_dir) {
    active_title = subtitler::LibrarySubtitleTitle(*state_dir, *subtitles);
  }

  if (whisper_model) {
    // The tap lives on the capture side's audio branch (#19).
    if (!audio) {
      std::println(stderr,
                   "--whisper needs the audio branch (--no-audio "
                   "was given)");
      return EXIT_FAILURE;
    }
    if (!std::filesystem::exists(*whisper_model)) {
      std::println(stderr, "Whisper model not found: {}", *whisper_model);
      return EXIT_FAILURE;
    }
  }

  auto stream = subtitler::Stream::Create(
      device, output_mode, connector_id, audio, audio_output_device,
      audio_offset_ms, web, subtitles, whisper_model);

  if (!stream) {
    std::println(stderr, "Failed to create stream");
    return EXIT_FAILURE;
  }

  // The persisted whisper state applies when --whisper didn't override
  // it; a configured model that's gone leaves the tap disabled (#220).
  if (!whisper_model && config && state_dir) {
    const auto& values = config->values();
    if (values.whisper_enabled.value_or(false) && values.whisper_model) {
      if (const auto path =
              subtitler::WhisperModelPath(*state_dir, *values.whisper_model);
          path && std::filesystem::is_regular_file(*path)) {
        if (!stream->SetWhisperState(true, path->string())) {
          MAIN_LOG(subtitler::LogLevel::kWarning,
                   "Could not enable whisper with {}", path->string());
        }
      } else {
        MAIN_LOG(subtitler::LogLevel::kWarning,
                 "Configured whisper model not found: {}",
                 *values.whisper_model);
      }
    }
  }

  // The persisted style (#222/#223) lands after the SRT is attached.
  if (config) {
    if (subtitles) {
      ApplySubtitleStyle(*stream, *config);
    }
    if (config->values().subtitles_visible) {
      stream->SetSubtitlesVisible(*config->values().subtitles_visible);
    }
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

    hooks.api_key = api_key;

    // Uploads (#212) and the library/state endpoints (#441): store into
    // the state-dir library, mark active for boot resume, and
    // live-switch the stream to the new SRT.
    if (state_dir) {
      hooks.subtitle_upload =
          [&stream, &state_dir, &active_title, &config](
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

        if (config) {
          config->SetSubtitleFile(stored->string());
          // The switch reset the delay; restore the persisted style.
          ApplySubtitleStyle(*stream, *config);
          if (!config->Save()) {
            MAIN_LOG(subtitler::LogLevel::kWarning,
                     "Could not save the configuration");
          }
        }

        return {subtitler::SubtitleUploadStatus::kStored,
                relative->generic_string()};
      };

      hooks.subtitle_get = [&state_dir](std::string_view title) {
        return subtitler::LoadLibrarySubtitle(*state_dir, title);
      };

      hooks.subtitle_list = [&state_dir] {
        return subtitler::ListSubtitles(*state_dir);
      };

      // Deletion (#453): removes the library entry; deleting the
      // attached subtitle detaches it like an empty state-set file.
      hooks.subtitle_delete =
          [&stream, &state_dir, &active_title,
           &config](std::string_view title) -> subtitler::SubtitleDeleteStatus {
        if (!subtitler::FindLibrarySubtitle(*state_dir, title)) {
          return subtitler::SubtitleDeleteStatus::kNotFound;
        }

        if (active_title.has_value() && *active_title == title) {
          if (!stream->SetSubtitleFile(std::nullopt)) {
            return subtitler::SubtitleDeleteStatus::kFailed;
          }
          subtitler::ClearActiveSubtitle(*state_dir);
          active_title = std::nullopt;
          if (config) {
            config->SetSubtitleFile(std::nullopt);
            if (!config->Save()) {
              MAIN_LOG(subtitler::LogLevel::kWarning,
                       "Could not save the configuration");
            }
          }
        }

        return subtitler::RemoveLibrarySubtitle(*state_dir, title)
                   ? subtitler::SubtitleDeleteStatus::kDeleted
                   : subtitler::SubtitleDeleteStatus::kFailed;
      };

      hooks.font_list = [] { return subtitler::AvailableFontFamilies(); };

      // The one-shot auto-sync (#433): the stream owns the session.
      hooks.subtitle_sync_get = [&stream] {
        const auto state = stream->SubtitleSync();
        subtitler::SubtitleSyncState result;
        switch (state.status) {
          case subtitler::Stream::SyncStatus::kIdle:
            result.status = subtitler::SubtitleSyncStatus::kIdle;
            break;
          case subtitler::Stream::SyncStatus::kListening:
            result.status = subtitler::SubtitleSyncStatus::kListening;
            break;
          case subtitler::Stream::SyncStatus::kSynced:
            result.status = subtitler::SubtitleSyncStatus::kSynced;
            result.time_ms = state.time_ms;
            break;
          case subtitler::Stream::SyncStatus::kFailed:
            result.status = subtitler::SubtitleSyncStatus::kFailed;
            if (!state.reason.empty()) {
              result.reason = state.reason;
            }
            break;
        }
        return result;
      };

      hooks.subtitle_sync_start = [&stream] {
        switch (stream->StartSubtitleSync()) {
          case subtitler::Stream::SyncStartResult::kStarted:
            return subtitler::SubtitleSyncStartResult::kStarted;
          case subtitler::Stream::SyncStartResult::kNoSubtitles:
            return subtitler::SubtitleSyncStartResult::kNoSubtitles;
          case subtitler::Stream::SyncStartResult::kNoWhisper:
            return subtitler::SubtitleSyncStartResult::kNoWhisper;
          case subtitler::Stream::SyncStartResult::kUnparseableSubtitles:
            return subtitler::SubtitleSyncStartResult::kUnparseableSubtitles;
        }
        return subtitler::SubtitleSyncStartResult::kNoSubtitles;
      };

      // The whisper tap (#19): state changes apply live on the stream
      // and persist to the config; the state dir anchors the model
      // store the routes list and store into.
      hooks.state_dir = *state_dir;

      hooks.whisper_state_get = [&stream] {
        subtitler::WhisperRouteState state;
        state.enabled = stream->WhisperEnabled();
        if (const auto model = stream->WhisperModel()) {
          state.model = std::filesystem::path{*model}.filename().string();
        }
        return state;
      };

      hooks.whisper_state_set =
          [&stream, &state_dir, &config](
              std::optional<bool> enabled,
              std::optional<std::string_view> model) -> bool {
        std::optional<std::string> path;
        if (model) {
          const auto resolved = subtitler::WhisperModelPath(*state_dir, *model);
          if (!resolved || !std::filesystem::is_regular_file(*resolved)) {
            return false;
          }
          path = resolved->string();
        }

        if (!stream->SetWhisperState(enabled.value_or(stream->WhisperEnabled()),
                                     path)) {
          return false;
        }

        if (config) {
          config->SetWhisperEnabled(stream->WhisperEnabled());
          if (model) {
            config->SetWhisperModel(*model);
          }
          if (!config->Save()) {
            MAIN_LOG(subtitler::LogLevel::kWarning,
                     "Could not save the configuration");
          }
        }

        return true;
      };

      hooks.subtitle_state_get = [&stream, &active_title] {
        return subtitler::SubtitleState{
            .file = active_title,
            .visible = stream->SubtitlesVisible(),
            .paused = stream->SubtitlesPaused(),
            .time_ms = stream->SubtitleTime().value_or(0),
            .delay_ms = stream->SubtitleDelay(),
            .font_family = stream->SubtitleFontFamily(),
            .font_size = stream->SubtitleFontSize(),
            .font_color = stream->SubtitleFontColor(),
        };
      };

      hooks.subtitle_state_set =
          [&stream, &state_dir, &active_title,
           &config](const subtitler::SubtitleStatePatch& patch) -> bool {
        if (patch.file) {
          if (patch.file->empty()) {
            if (!stream->SetSubtitleFile(std::nullopt)) {
              return false;
            }
            subtitler::ClearActiveSubtitle(*state_dir);
            active_title = std::nullopt;
            if (config) {
              config->SetSubtitleFile(std::nullopt);
            }
          } else {
            const auto relative =
                subtitler::FindLibrarySubtitle(*state_dir, *patch.file);
            if (!relative ||
                !stream->SetSubtitleFile(
                    (*state_dir / "subtitles" / *relative).string())) {
              return false;
            }
            if (!subtitler::SetActiveSubtitle(*state_dir,
                                              relative->generic_string())) {
              MAIN_LOG(subtitler::LogLevel::kWarning,
                       "Could not write the active subtitle marker");
            }
            active_title = *patch.file;
            if (config) {
              config->SetSubtitleFile(
                  (*state_dir / "subtitles" / *relative).string());
              // The switch reset the delay; restore the persisted style.
              // Patch-supplied fields below still override it.
              ApplySubtitleStyle(*stream, *config);
            }
          }
        }

        // After a file switch: it restarts at position 0 with the delay
        // reset, so the remaining changes apply afterwards.
        if (patch.visible) {
          stream->SetSubtitlesVisible(*patch.visible);
          if (config) {
            config->SetSubtitlesVisible(*patch.visible);
          }
        }
        if (patch.paused) {
          stream->SetSubtitlesPaused(*patch.paused);
        }
        if (patch.time_ms) {
          stream->SetSubtitleTime(*patch.time_ms);
        }
        if (patch.delay_ms) {
          stream->SetSubtitleDelay(*patch.delay_ms);
          if (config) {
            config->SetSubtitleDelayMs(*patch.delay_ms);
          }
        }
        if (patch.font_family) {
          stream->SetSubtitleFontFamily(*patch.font_family);
          if (config) {
            config->SetSubtitleFontFamily(*patch.font_family);
          }
        }
        if (patch.font_size) {
          stream->SetSubtitleFontSize(*patch.font_size);
          if (config) {
            config->SetSubtitleFontSizePt(*patch.font_size);
          }
        }
        if (patch.font_color) {
          stream->SetSubtitleFontColor(*patch.font_color);
          if (config) {
            config->SetSubtitleFontColor(*patch.font_color);
          }
        }

        // Position and pause are playback state, not configuration.
        if (config &&
            (patch.file || patch.visible || patch.delay_ms ||
             patch.font_family || patch.font_size || patch.font_color)) {
          if (!config->Save()) {
            MAIN_LOG(subtitler::LogLevel::kWarning,
                     "Could not save the configuration");
          }
        }

        return true;
      };
    }

    web_server = subtitler::WebServer::Create(kWebPort, stream->PreviewFrames(),
                                              std::move(hooks));

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
