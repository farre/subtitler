#pragma once

#include <chrono>
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

// What one full window transcribed to.
struct Transcription {
  std::string text;
  // The window's trailing silence: the speech in text ends this far
  // before the window's own end, so timestamps taken from the window
  // end can be moved back to the speech (0 when speech runs to the
  // end, the whole window when nothing was transcribed).
  std::chrono::milliseconds trailing_silence{0};
};

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

  // Feeds 16 kHz mono samples. Returns the window's transcription each
  // time a full window has been transcribed (empty text when the
  // window held nothing transcribable), nullopt while still
  // accumulating.
  std::optional<Transcription> Push(std::span<const float> samples);

 private:
  explicit WhisperTranscriber(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
