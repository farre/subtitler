#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "sync/srt_cues.h"

namespace subtitler {

// A timestamped piece of text: the text itself and the running time
// at which its audio ended (capture PTS in the shared timeline
// domain, ns).
struct TimestampedText {
  std::string text;
  std::int64_t timestamp_ns = 0;
};

// The recognition-matching seam (#18): maps collected text windows
// onto the SRT timeline, answering the clock offset θ (ns):
// srt_position = running_time + θ. nullopt while the evidence is too
// weak or too ambiguous (#21). Replaceable without touching the
// session — alignment and tracking are testable without a transcriber.
using SyncMatcher = std::function<std::optional<std::int64_t>(
    const std::vector<SrtCue>&, const std::vector<TimestampedText>&)>;

// A one-shot auto-sync session (#433): windows are fed as the
// transcriber produces them; the session locks when the matcher finds
// a stable offset and fails at the deadline. Pure bookkeeping, no Gst
// — the stream feeds it and applies the result.
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

  // Feeds one text window (now_ns = current running time) and answers
  // the state after the feed.
  Result Feed(TimestampedText window, std::int64_t now_ns);

  // The state without new input; fails once the deadline has passed.
  Result Poll(std::int64_t now_ns) const;

 private:
  std::vector<SrtCue> cues_;
  SyncMatcher matcher_;
  std::int64_t deadline_ns_;
  std::vector<TimestampedText> windows_;
};

}  // namespace subtitler
