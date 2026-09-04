#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sync/srt_cues.h"
#include "sync/sync_session.h"

namespace subtitler {

// #290's normalization, shared by cues and transcripts alike: lowercase,
// every non-alphanumeric run becomes one space, split into words.
std::vector<std::string> NormalizeMatchWords(std::string_view text);

// Why a transcript window did not vote for a clock offset.
enum class WindowReject : std::uint8_t {
  kNone,            // it voted
  kTooShort,        // fewer than three normalized words
  kBelowThreshold,  // the winning word offset collected too few hits
  kAmbiguous,       // the runner-up offset was too close to the winner
  kOutOfRange,      // the winning offset mapped outside the SRT stream
};

// How one transcript window matched the SRT (#21): the raw numbers
// behind MatchTranscript's per-window decision, exposed for matcher
// development tooling (subtitler-test).
struct WindowMatch {
  // Normalized word count of the window.
  std::size_t words = 0;
  // Trigram hits collected by the winning word offset, and by the
  // runner-up (0 when no other offset collected any).
  int best_hits = 0;
  int runner_up_hits = 0;
  WindowReject reject = WindowReject::kNone;
  // The window's voted clock offset θ (ns): srt_position =
  // running_time + θ. Set when reject is kNone.
  std::optional<std::int64_t> theta_ns;
};

// One window's vote, with the voting numbers. Rebuilds the SRT index
// per call — a tooling entry point; MatchTranscript is the production
// one (both share the same voting implementation).
WindowMatch MatchWindow(const std::vector<SrtCue>& cues,
                        const TimestampedText& window);

// The production SyncMatcher (#21): word-trigram voting over the
// normalized (#290) cue and transcript word streams. Each window votes
// for a word offset; a window's vote counts only above a hit threshold
// and a 2x margin over the runner-up, so repeated common phrases can't
// lock. θ answers only once a cluster of windows agrees (median of the
// tightest run, spread at most 2 s).
std::optional<std::int64_t> MatchTranscript(
    const std::vector<SrtCue>& cues,
    const std::vector<TimestampedText>& windows);

}  // namespace subtitler
