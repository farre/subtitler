#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace subtitler {

// One parsed SRT cue: times in milliseconds, text with markup stripped
// and the cue's lines joined by '\n'.
struct SrtCue {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
};

// Tolerant SRT parsing for sync matching (#433): valid cues are kept,
// malformed blocks skipped. Handles CRLF and LF, multiline cues, an
// optional sequence-number line, position attributes after the end
// time, and strips simple markup (<i>, <b>, <font ...>, ...). Times use
// the SRT "HH:MM:SS,mmm" shape (a dot for the decimal separator is
// tolerated).
std::vector<SrtCue> ParseSrtCues(std::string_view srt);

}  // namespace subtitler
