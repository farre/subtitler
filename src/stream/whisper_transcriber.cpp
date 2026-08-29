#include "stream/whisper_transcriber.h"

#include <chrono>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/logging.h"
#include "utils/unique_ptr.h"

#ifdef SUBTITLER_ENABLE_WHISPER
#include <whisper.h>
#endif

namespace subtitler {

#ifdef SUBTITLER_ENABLE_WHISPER

namespace {

constexpr int kSampleRate = 16000;
// Whisper's streaming sweet spot: long enough for context, short enough
// that a window's inference stays well under the window's duration on
// the Pi 5 (the spike's timing logs test exactly that).
constexpr std::size_t kWindowSeconds = 5;
constexpr std::size_t kWindowSamples = kSampleRate * kWindowSeconds;
// Leaves cores for the passthrough on the 4-core Pi 5.
constexpr int kThreads = 2;

// Routes whisper.cpp's own logging (the model-load banner is INFO) into
// the stream label: errors/warnings surface, the rest is debug noise.
void WhisperLogCallback(enum ggml_log_level level, const char* text,
                        void* /*user_data*/) {
  try {
    std::string_view message{text};
    if (!message.empty() && message.back() == '\n') {
      message.remove_suffix(1);
    }

    switch (level) {
      case GGML_LOG_LEVEL_ERROR:
        STREAM_LOG(LogLevel::kError, "whisper: {}", message);
        break;
      case GGML_LOG_LEVEL_WARN:
        STREAM_LOG(LogLevel::kWarning, "whisper: {}", message);
        break;
      default:
        STREAM_LOG(LogLevel::kDebug, "whisper: {}", message);
        break;
    }
  } catch (...) {
    // A logging callback must never throw into the C API.
  }
}

}  // namespace

struct WhisperTranscriber::Implementation {
  UniquePtr<whisper_context, whisper_free> context;
  std::vector<float> window;
};

/* static */
std::unique_ptr<WhisperTranscriber> WhisperTranscriber::Create(
    const std::string& model_path) {
  // Global, idempotent; there is one transcriber per process.
  whisper_log_set(WhisperLogCallback, nullptr);

  auto implementation = std::make_unique<Implementation>();
  implementation->context.reset(whisper_init_from_file_with_params(
      model_path.c_str(), whisper_context_default_params()));

  if (!implementation->context) {
    return nullptr;
  }

  implementation->window.reserve(kWindowSamples);
  return std::unique_ptr<WhisperTranscriber>{
      new WhisperTranscriber{std::move(implementation)}};
}

WhisperTranscriber::WhisperTranscriber(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

WhisperTranscriber::~WhisperTranscriber() = default;

std::optional<std::string> WhisperTranscriber::Push(
    std::span<const float> samples) {
  auto& window = implementation_->window;
  window.insert(window.end(), samples.begin(), samples.end());

  if (window.size() < kWindowSamples) {
    return std::nullopt;
  }

  auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.print_realtime = false;
  params.print_progress = false;
  params.print_timestamps = false;
  params.print_special = false;
  params.translate = false;
  params.language = "en";
  params.n_threads = kThreads;
  // Windowed mode (like whisper.cpp's stream example): no cross-window
  // context, no timestamp tokens — one text per window.
  params.no_context = true;
  params.no_timestamps = true;
  // The mel hop is 160 samples; sizing the encoder context to the window
  // (instead of the 30 s default) cuts the encoder's work accordingly.
  params.audio_ctx = static_cast<int>(kWindowSamples / 160);

  const auto started = std::chrono::steady_clock::now();

  const int result = whisper_full(implementation_->context.get(), params,
                                  window.data(), kWindowSamples);

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (result != 0) {
    STREAM_LOG(LogLevel::kWarning, "Whisper transcription failed ({})", result);
    window.clear();
    return std::nullopt;
  }

  // Keep any backlog as the start of the next window; the tap's dropping
  // appsink bounds how far behind live audio this can fall.
  window.erase(window.begin(), window.begin() + kWindowSamples);

  std::string text;
  const int segments = whisper_full_n_segments(implementation_->context.get());
  for (int i = 0; i < segments; ++i) {
    text += whisper_full_get_segment_text(implementation_->context.get(), i);
  }

  STREAM_LOG(LogLevel::kDebug, "Whisper transcribed a {} s window in {}",
             kWindowSeconds, elapsed);

  return text;
}

#else  // SUBTITLER_ENABLE_WHISPER

struct WhisperTranscriber::Implementation {};

/* static */
std::unique_ptr<WhisperTranscriber> WhisperTranscriber::Create(
    const std::string&) {
  std::println(stderr,
               "Whisper support is not compiled in; configure with "
               "-DSUBTITLER_ENABLE_WHISPER=ON");
  return nullptr;
}

WhisperTranscriber::WhisperTranscriber(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

WhisperTranscriber::~WhisperTranscriber() = default;

std::optional<std::string> WhisperTranscriber::Push(std::span<const float>) {
  return std::nullopt;
}

#endif  // SUBTITLER_ENABLE_WHISPER

}  // namespace subtitler
