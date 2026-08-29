#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "stream/srt_cues.h"
#include "stream/sync_session.h"

namespace subtitler {

// The production SyncMatcher (#21): word-trigram voting over the
// normalized (#290) cue and transcript word streams. Each window votes
// for a word offset; a window's vote counts only above a hit threshold
// and a 2x margin over the runner-up, so repeated common phrases can't
// lock. θ answers only once a cluster of windows agrees (median of the
// tightest run, spread at most 2 s).
std::optional<std::int64_t> MatchTranscript(
    const std::vector<SrtCue>& cues, const std::vector<WhisperWindow>& windows);

}  // namespace subtitler
