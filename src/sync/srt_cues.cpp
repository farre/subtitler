#include "sync/srt_cues.h"

#include <charconv>
#include <optional>

namespace subtitler {

namespace {

// "HH:MM:SS,mmm" (or with a dot); hours are unbounded.
std::optional<std::int64_t> ParseTimestamp(std::string_view text) {
  const auto read = [&text](std::size_t at, std::size_t count,
                            int& value) -> bool {
    if (at + count > text.size()) {
      return false;
    }
    const auto [end, error] =
        std::from_chars(text.data() + at, text.data() + at + count, value);
    return error == std::errc{} && end == text.data() + at + count;
  };

  int hours, minutes, seconds, millis;
  if (!read(0, 2, hours) || text.size() < 12 || text[2] != ':' ||
      !read(3, 2, minutes) || text[5] != ':' || !read(6, 2, seconds) ||
      (text[8] != ',' && text[8] != '.') || !read(9, 3, millis)) {
    return std::nullopt;
  }

  return (static_cast<std::int64_t>(hours) * 3600 + minutes * 60 + seconds) *
             1000 +
         millis;
}

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// Strips "<...>" runs: SRT's simple markup (<i>, </b>, <font ...>).
std::string StripMarkup(std::string_view text) {
  std::string stripped;
  stripped.reserve(text.size());
  bool in_tag = false;
  for (const char c : text) {
    if (c == '<') {
      in_tag = true;
    } else if (c == '>') {
      in_tag = false;
    } else if (!in_tag) {
      stripped += c;
    }
  }
  return stripped;
}

void ParseBlock(std::string_view block, std::vector<SrtCue>& cues) {
  SrtCue cue;
  bool timed = false;

  std::string text;
  while (!block.empty()) {
    const auto newline = block.find('\n');
    const std::string_view line = Trim(block.substr(
        0, newline == std::string_view::npos ? block.size() : newline));
    block.remove_prefix(newline == std::string_view::npos ? block.size()
                                                          : newline + 1);

    if (!timed) {
      const auto arrow = line.find("-->");
      if (arrow == std::string_view::npos) {
        // The sequence-number line, or garbage before the timing line.
        continue;
      }

      const auto start = ParseTimestamp(Trim(line.substr(0, arrow)));
      // Anything after the end timestamp (position attributes) goes.
      auto tail = Trim(line.substr(arrow + 3));
      if (const auto space = tail.find_first_of(" \t");
          space != std::string_view::npos) {
        tail = tail.substr(0, space);
      }
      const auto end = ParseTimestamp(tail);

      if (!start || !end || *end <= *start) {
        return;
      }

      cue.start_ms = *start;
      cue.end_ms = *end;
      timed = true;
    } else {
      if (!text.empty()) {
        text += '\n';
      }
      text += StripMarkup(line);
    }
  }

  if (timed && !Trim(text).empty()) {
    cue.text = std::move(text);
    cues.push_back(std::move(cue));
  }
}

}  // namespace

std::vector<SrtCue> ParseSrtCues(std::string_view srt) {
  std::vector<SrtCue> cues;

  // CRLF normalization: blocks split on "\n\n", which "\r\n\r\n"
  // breaks.
  std::string normalized;
  if (srt.find('\r') != std::string_view::npos) {
    normalized.reserve(srt.size());
    for (const char c : srt) {
      if (c != '\r') {
        normalized += c;
      }
    }
    srt = normalized;
  }

  // Blocks are separated by blank lines.
  std::size_t at = 0;
  while (at < srt.size()) {
    const auto next = srt.find("\n\n", at);
    auto block = srt.substr(
        at, next == std::string_view::npos ? srt.size() - at : next - at);
    if (!Trim(block).empty()) {
      ParseBlock(block, cues);
    }
    at = next == std::string_view::npos ? srt.size() : next + 2;
  }

  return cues;
}

}  // namespace subtitler
