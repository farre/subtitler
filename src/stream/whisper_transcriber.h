#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace subtitler {

// Whether the whisper tap is compiled in (SUBTITLER_ENABLE_WHISPER,
// default ON). Without it the transcriber is a stub that fails Create,
// so the rest of the tree needs no ifdefs beyond this constant.
#ifdef SUBTITLER_ENABLE_WHISPER
inline constexpr bool kWhisperAvailable = true;
#else
inline constexpr bool kWhisperAvailable = false;
#endif

// Windowed speech-to-text over 16 kHz mono float samples — the caps of
// the whisper tap's appsink (#19 spike, #266). Push accumulates samples;
// every full window is transcribed and its text returned. Compiles to a
// stub that fails Create unless built with SUBTITLER_ENABLE_WHISPER.
//
// Not thread-safe: feed it from a single thread.
class WhisperTranscriber {
  struct Implementation;

 public:
  // Loads the ggml model; nullptr when the model can't be loaded (or
  // whisper support isn't compiled in).
  static std::unique_ptr<WhisperTranscriber> Create(
      const std::string& model_path);
  ~WhisperTranscriber();

  WhisperTranscriber(const WhisperTranscriber&) = delete;
  WhisperTranscriber& operator=(const WhisperTranscriber&) = delete;

  // Feeds 16 kHz mono samples. Returns the window's text each time a full
  // window has been transcribed (empty when the window held nothing
  // transcribable), nullopt while still accumulating.
  std::optional<std::string> Push(std::span<const float> samples);

 private:
  explicit WhisperTranscriber(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
