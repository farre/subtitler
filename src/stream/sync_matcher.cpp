#include "stream/sync_matcher.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace subtitler {

namespace {

// #290's normalization, shared by cues and transcripts alike: lowercase,
// every non-alphanumeric run becomes one space, split into words.
std::vector<std::string> NormalizeWords(std::string_view text) {
  std::vector<std::string> words;
  std::string word;
  for (const unsigned char c : text) {
    if (std::isalnum(c) != 0) {
      word += static_cast<char>(std::tolower(c));
    } else if (!word.empty()) {
      words.push_back(std::move(word));
      word.clear();
    }
  }
  if (!word.empty()) {
    words.push_back(std::move(word));
  }
  return words;
}

// A word in the flattened SRT stream, with its cue's time bounds for
// the interpolation back onto the timeline.
struct SrtWord {
  std::string word;
  std::int64_t start_ms;
  std::int64_t end_ms;
  // This word's fraction through its cue (first word 0, last ~1).
  double position;
};

std::vector<SrtWord> FlattenCues(const std::vector<SrtCue>& cues) {
  std::vector<SrtWord> stream;
  for (const auto& cue : cues) {
    const auto words = NormalizeWords(cue.text);
    const double count = static_cast<double>(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
      stream.push_back({words[i], cue.start_ms, cue.end_ms,
                        count > 1.0 ? i / (count - 1.0) : 0.0});
    }
  }
  return stream;
}

// A window's vote for the clock offset θ (ns): the winning word offset
// must collect kMinHits trigrams with a kMargin over the runner-up, so
// repeated phrases and error noise can't win.
constexpr int kMinHits = 4;
constexpr int kMargin = 2;
// The lock: at least this many windows must vote, and the tightest
// agreeing run may spread at most this far.
constexpr std::size_t kMinVotes = 3;
constexpr std::int64_t kMaxSpreadNs = 2'000'000'000;

std::optional<std::int64_t> VoteWindow(
    const WhisperWindow& window, const std::vector<SrtWord>& stream,
    const std::unordered_map<std::string, std::vector<std::size_t>>& index) {
  const auto words = NormalizeWords(window.text);
  if (words.size() < 3) {
    return std::nullopt;
  }

  std::map<std::ptrdiff_t, int> tally;
  for (std::size_t i = 0; i + 2 < words.size(); ++i) {
    const std::string trigram =
        words[i] + ' ' + words[i + 1] + ' ' + words[i + 2];
    const auto found = index.find(trigram);
    if (found == index.end()) {
      continue;
    }
    for (const std::size_t at : found->second) {
      ++tally[static_cast<std::ptrdiff_t>(at) - static_cast<std::ptrdiff_t>(i)];
    }
  }

  const auto best = std::ranges::max_element(
      tally, [](const auto& a, const auto& b) { return a.second < b.second; });
  if (best == tally.end() || best->second < kMinHits) {
    return std::nullopt;
  }
  for (const auto& [offset, count] : tally) {
    if (offset != best->first && best->second < kMargin * count) {
      // Ambiguous: the runner-up is too close to the winner.
      return std::nullopt;
    }
  }

  // The window's last word maps to its offset in the SRT stream; its
  // time is interpolated across its cue. That instant is what the
  // window's audio_end is the running time of, so θ = srt - running.
  const std::ptrdiff_t last =
      best->first + static_cast<std::ptrdiff_t>(words.size()) - 1;
  if (last < 0 || static_cast<std::size_t>(last) >= stream.size()) {
    return std::nullopt;
  }
  const SrtWord& word = stream[last];
  const auto word_ms = static_cast<std::int64_t>(
      word.start_ms + word.position * (word.end_ms - word.start_ms));
  return word_ms * 1'000'000 - window.audio_end_ns;
}

}  // namespace

std::optional<std::int64_t> MatchTranscript(
    const std::vector<SrtCue>& cues,
    const std::vector<WhisperWindow>& windows) {
  const auto stream = FlattenCues(cues);
  if (stream.size() < 3) {
    return std::nullopt;
  }

  // Trigram -> the word indices it starts at.
  std::unordered_map<std::string, std::vector<std::size_t>> index;
  for (std::size_t i = 0; i + 2 < stream.size(); ++i) {
    index[stream[i].word + ' ' + stream[i + 1].word + ' ' + stream[i + 2].word]
        .push_back(i);
  }

  std::vector<std::int64_t> votes;
  for (const auto& window : windows) {
    if (const auto vote = VoteWindow(window, stream, index)) {
      votes.push_back(*vote);
    }
  }

  if (votes.size() < kMinVotes) {
    return std::nullopt;
  }

  std::ranges::sort(votes);

  // The tightest agreeing run of at least kMinVotes votes.
  std::size_t best_begin = 0;
  std::size_t best_size = 0;
  for (std::size_t begin = 0; begin < votes.size(); ++begin) {
    std::size_t end = begin;
    while (end + 1 < votes.size() &&
           votes[end + 1] - votes[begin] <= kMaxSpreadNs) {
      ++end;
    }
    if (end - begin + 1 > best_size) {
      best_begin = begin;
      best_size = end - begin + 1;
    }
  }

  if (best_size < kMinVotes) {
    return std::nullopt;
  }

  return votes[best_begin + best_size / 2];
}

}  // namespace subtitler
