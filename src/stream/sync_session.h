#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "stream/srt_cues.h"

namespace subtitler {

// One transcribed whisper window: the text of ~5 s of audio and the
// running time at which that audio ended (capture PTS in the shared
// timeline domain, ns).
struct WhisperWindow {
  std::string text;
  std::int64_t audio_end_ns = 0;
};

// The recognition-matching seam (#18): maps collected transcript
// windows onto the SRT timeline, answering the clock offset θ (ns):
// srt_position = running_time + θ. nullopt while the evidence is too
// weak or too ambiguous (#21). Replaceable without touching the
// session — alignment and tracking are testable without whisper.
using SyncMatcher = std::function<std::optional<std::int64_t>(
    const std::vector<SrtCue>&, const std::vector<WhisperWindow>&)>;

// A one-shot auto-sync session (#433): windows are fed as whisper
// produces them; the session locks when the matcher finds a stable
// offset and fails at the deadline. Pure bookkeeping, no Gst — the
// stream feeds it and applies the result.
class SyncSession {
 public:
  enum class State : std::uint8_t { kListening, kSynced, kFailed };

  struct Result {
    State state = State::kListening;
    // The matched SRT position at lock time in ms; set when synced.
    std::optional<std::int64_t> time_ms;
    // Why the session failed; set when failed.
    std::string reason;
  };

  SyncSession(std::vector<SrtCue> cues, SyncMatcher matcher,
              std::int64_t deadline_ns);

  // Feeds one transcript window (now_ns = current running time) and
  // answers the state after the feed.
  Result Feed(WhisperWindow window, std::int64_t now_ns);

  // The state without new input; fails once the deadline has passed.
  Result Poll(std::int64_t now_ns) const;

 private:
  std::vector<SrtCue> cues_;
  SyncMatcher matcher_;
  std::int64_t deadline_ns_;
  std::vector<WhisperWindow> windows_;
};

}  // namespace subtitler
