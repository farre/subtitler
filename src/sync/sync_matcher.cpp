#include "sync/sync_matcher.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "utils/logging.h"

namespace subtitler {

namespace {

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
    const auto words = NormalizeMatchWords(cue.text);
    const double count = static_cast<double>(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
      stream.push_back({words[i], cue.start_ms, cue.end_ms,
                        count > 1.0 ? i / (count - 1.0) : 0.0});
    }
  }
  return stream;
}

// The SRT word stream plus its trigram index (trigram -> the word
// indices it starts at), built once per matching pass.
struct SrtIndex {
  std::vector<SrtWord> stream;
  std::unordered_map<std::string, std::vector<std::size_t>> trigrams;
};

SrtIndex IndexCues(const std::vector<SrtCue>& cues) {
  SrtIndex index{.stream = FlattenCues(cues), .trigrams = {}};
  for (std::size_t i = 0; i + 2 < index.stream.size(); ++i) {
    index
        .trigrams[index.stream[i].word + ' ' + index.stream[i + 1].word + ' ' +
                  index.stream[i + 2].word]
        .push_back(i);
  }
  return index;
}

// A window's vote for the clock offset θ (ns): the winning word-offset
// region must collect kMinScore rarity-weighted trigram hits with a
// kMargin over the runner-up, so repeated common phrases and error
// noise can't win. Each trigram weighs 1/occurrences — a phrase unique
// to the movie scores 1.0, a stock phrase occurring ten times 0.1 —
// and kMinScore asks for the equivalent of two unique anchors.
constexpr double kMinScore = 2.0;
constexpr int kMargin = 2;
// The lock: at least this many windows must vote, and the tightest
// agreeing run may spread at most this far.
constexpr std::size_t kMinVotes = 3;
constexpr std::int64_t kMaxSpreadNs = 2'000'000'000;

WindowMatch VoteWindow(const TimestampedText& window, const SrtIndex& index) {
  const auto words = NormalizeMatchWords(window.text);
  WindowMatch match;
  match.words = words.size();
  if (words.size() < 3) {
    match.reject = WindowReject::kTooShort;
    return match;
  }

  std::map<std::ptrdiff_t, int> hits;
  std::map<std::ptrdiff_t, double> score;
  for (std::size_t i = 0; i + 2 < words.size(); ++i) {
    const std::string trigram =
        words[i] + ' ' + words[i + 1] + ' ' + words[i + 2];
    const auto found = index.trigrams.find(trigram);
    if (found == index.trigrams.end()) {
      continue;
    }
    const double weight = 1.0 / static_cast<double>(found->second.size());
    for (const std::size_t at : found->second) {
      const auto offset =
          static_cast<std::ptrdiff_t>(at) - static_cast<std::ptrdiff_t>(i);
      ++hits[offset];
      score[offset] += weight;
    }
  }

  // Merge the buckets within one word of an offset: a single inserted
  // or omitted word shifts every later trigram by one, splitting a
  // window's evidence between two offsets that describe the same
  // region.
  const auto region_hits = [&](std::ptrdiff_t at) {
    int total = 0;
    for (const auto offset : {at - 1, at, at + 1}) {
      if (const auto it = hits.find(offset); it != hits.end()) {
        total += it->second;
      }
    }
    return total;
  };
  const auto region_score = [&](std::ptrdiff_t at) {
    double total = 0;
    for (const auto offset : {at - 1, at, at + 1}) {
      if (const auto it = score.find(offset); it != score.end()) {
        total += it->second;
      }
    }
    return total;
  };

  // The winner: the highest-scoring region, ties to the lowest offset.
  std::ptrdiff_t best_at = 0;
  double best_score = 0;
  for (const auto& [offset, _] : score) {
    if (const double total = region_score(offset); total > best_score) {
      best_at = offset;
      best_score = total;
    }
  }
  match.best_hits = region_hits(best_at);
  match.best_score = best_score;
  if (best_score < kMinScore) {
    SYNC_LOG(LogLevel::kDebug,
             "Subtitle sync window below threshold: best region scored "
             "{:.2f} ({} hits)",
             match.best_score, match.best_hits);
    match.reject = WindowReject::kBelowThreshold;
    return match;
  }

  // The runner-up: the best region that shares no buckets with the
  // winner.
  for (const auto& [offset, _] : score) {
    if (std::abs(offset - best_at) <= 2) {
      continue;
    }
    if (const double total = region_score(offset);
        total > match.runner_up_score) {
      match.runner_up_score = total;
      match.runner_up_hits = region_hits(offset);
    }
  }
  if (match.best_score < kMargin * match.runner_up_score) {
    // Ambiguous: the runner-up is too close to the winner.
    SYNC_LOG(LogLevel::kDebug,
             "Subtitle sync window ambiguous: {:.2f} vs runner-up {:.2f}",
             match.best_score, match.runner_up_score);
    match.reject = WindowReject::kAmbiguous;
    return match;
  }

  // The region's representative for θ: its strongest raw bucket (ties
  // to the lowest offset; a wrong pick costs one word, which the
  // cluster tolerance absorbs).
  std::ptrdiff_t vote_at = best_at;
  int vote_hits = 0;
  for (const auto offset : {best_at - 1, best_at, best_at + 1}) {
    if (const auto it = hits.find(offset);
        it != hits.end() && it->second > vote_hits) {
      vote_at = offset;
      vote_hits = it->second;
    }
  }

  // The window's last word maps to its offset in the SRT stream; its
  // time is interpolated across its cue. That instant is what the
  // window's timestamp is the running time of, so θ = srt - running.
  const std::ptrdiff_t last =
      vote_at + static_cast<std::ptrdiff_t>(words.size()) - 1;
  if (last < 0 || static_cast<std::size_t>(last) >= index.stream.size()) {
    match.reject = WindowReject::kOutOfRange;
    return match;
  }
  const SrtWord& word = index.stream[last];
  const auto word_ms = static_cast<std::int64_t>(
      word.start_ms + word.position * (word.end_ms - word.start_ms));
  match.theta_ns = word_ms * 1'000'000 - window.timestamp_ns;
  SYNC_LOG(LogLevel::kDebug,
           "Subtitle sync window voted offset {} ms ({} hits, score {:.2f})",
           *match.theta_ns / 1'000'000, match.best_hits, match.best_score);
  return match;
}

}  // namespace

std::vector<std::string> NormalizeMatchWords(std::string_view text) {
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

WindowMatch MatchWindow(const std::vector<SrtCue>& cues,
                        const TimestampedText& window) {
  return VoteWindow(window, IndexCues(cues));
}

std::optional<std::int64_t> MatchTranscript(
    const std::vector<SrtCue>& cues,
    const std::vector<TimestampedText>& windows) {
  const auto index = IndexCues(cues);
  if (index.stream.size() < 3) {
    return std::nullopt;
  }

  std::vector<std::int64_t> votes;
  for (const auto& window : windows) {
    const auto match = VoteWindow(window, index);
    if (match.theta_ns) {
      votes.push_back(*match.theta_ns);
    }
  }
  SYNC_LOG(LogLevel::kDebug, "Subtitle sync matching: {} of {} windows voted",
           votes.size(), windows.size());

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
    SYNC_LOG(LogLevel::kDebug,
             "Subtitle sync matching: votes too scattered to lock ({} "
             "votes, best run {})",
             votes.size(), best_size);
    return std::nullopt;
  }

  const std::int64_t theta_ns = votes[best_begin + best_size / 2];
  SYNC_LOG(LogLevel::kDebug,
           "Subtitle sync matching locked: {} votes agree, offset {} ms",
           best_size, theta_ns / 1'000'000);
  return theta_ns;
}

}  // namespace subtitler
