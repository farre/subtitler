#pragma once

#include <optional>

namespace subtitler {

// Logical demand, separate from the tap's physical enabled state. Guarded
// by Stream's sync_mutex_. Cleanup must test Wanted() while holding both
// whisper_mutex_ and sync_mutex_; a queued unconditional disable can race
// a new session or an explicit enable.
class WhisperDemand {
 public:
  void SetContinuous(std::optional<bool> enabled) {
    if (enabled) {
      continuous_ = *enabled;
    }
  }
  void StartSession() { session_ = true; }
  void EndSession() { session_ = false; }
  bool Wanted() const { return continuous_ || session_; }
  bool Continuous() const { return continuous_; }

 private:
  bool continuous_ = false;
  bool session_ = false;
};

}  // namespace subtitler
