#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "stream/sync_matcher.h"

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kMillisecond = 1'000'000;

// A vocabulary of unique words, six per cue, so every trigram occurs
// exactly once in the document.
std::vector<std::string> MakeWords(int count) {
  std::vector<std::string> words;
  for (int i = 0; i < count; ++i) {
    words.push_back(std::format("w{}", i));
  }
  return words;
}

std::vector<subtitler::SrtCue> MakeCues(const std::vector<std::string>& words,
                                        int words_per_cue, int cue_ms) {
  std::vector<subtitler::SrtCue> cues;
  for (std::size_t at = 0; at + words_per_cue <= words.size();
       at += words_per_cue) {
    subtitler::SrtCue cue;
    cue.start_ms = static_cast<std::int64_t>(cues.size()) * cue_ms;
    cue.end_ms = cue.start_ms + cue_ms - 500;
    for (int i = 0; i < words_per_cue; ++i) {
      if (!cue.text.empty()) {
        cue.text += ' ';
      }
      cue.text += words[at + i];
    }
    cues.push_back(std::move(cue));
  }
  return cues;
}

// The transcript of the given cue range as one whisper window whose
// audio ends at the last cue's end, shifted by the clock offset θ.
subtitler::WhisperWindow WindowAt(const std::vector<subtitler::SrtCue>& cues,
                                  std::size_t first, std::size_t last,
                                  std::int64_t theta_ns) {
  std::string text;
  for (std::size_t i = first; i <= last; ++i) {
    if (!text.empty()) {
      text += ' ';
    }
    text += cues[i].text;
  }
  return {text, cues[last].end_ms * kMillisecond - theta_ns};
}

// tiny.en style corruption: replaces every n-th word with noise.
std::string Corrupt(std::string text, int every, std::string_view noise) {
  std::string corrupted;
  std::string word;
  int index = 0;
  const auto flush = [&] {
    if (word.empty()) {
      return;
    }
    if (!corrupted.empty()) {
      corrupted += ' ';
    }
    corrupted += (++index % every == 0) ? noise : word;
    word.clear();
  };
  for (const char c : text) {
    if (c == ' ' || c == '\n') {
      flush();
    } else {
      word += c;
    }
  }
  flush();
  return corrupted;
}

}  // namespace

TEST_CASE("sync matcher") {
  const auto words = MakeWords(1200);
  const auto cues = MakeCues(words, 6, 3000);

  SUBCASE("locks on clean windows") {
    constexpr std::int64_t theta = 123 * kSecond;
    const std::vector<subtitler::WhisperWindow> windows = {
        WindowAt(cues, 10, 12, theta),
        WindowAt(cues, 40, 42, theta),
        WindowAt(cues, 70, 72, theta),
    };

    const auto matched = subtitler::MatchTranscript(cues, windows);
    REQUIRE(matched.has_value());
    // The exact offset the windows were built with.
    CHECK(*matched == theta);
  }

  SUBCASE("locks through tiny.en-style word noise") {
    constexpr std::int64_t theta = 123 * kSecond;
    std::vector<subtitler::WhisperWindow> windows;
    for (const auto [first, last] :
         {std::pair{10UL, 12UL}, {40UL, 42UL}, {70UL, 72UL}}) {
      auto window = WindowAt(cues, first, last, theta);
      window.text = Corrupt(window.text, 5, "um");
      windows.push_back(std::move(window));
    }

    const auto matched = subtitler::MatchTranscript(cues, windows);
    REQUIRE(matched.has_value());
    CHECK(*matched == theta);
  }

  SUBCASE("unrelated transcripts never lock") {
    const std::vector<subtitler::WhisperWindow> windows = {
        {"the quick brown fox jumps over", kSecond},
        {"a lazy dog keeps sleeping soundly", 2 * kSecond},
        {"pack my box with five dozen jugs", 3 * kSecond},
    };

    CHECK_FALSE(subtitler::MatchTranscript(cues, windows).has_value());
  }

  SUBCASE("one outlier cannot break an agreeing cluster") {
    constexpr std::int64_t theta = 123 * kSecond;
    const std::vector<subtitler::WhisperWindow> windows = {
        WindowAt(cues, 10, 12, theta),
        WindowAt(cues, 40, 42, theta),
        // This window's audio end is off by 10 s: an outlier vote.
        WindowAt(cues, 70, 72, theta + 10 * kSecond),
        WindowAt(cues, 100, 102, theta),
    };

    const auto matched = subtitler::MatchTranscript(cues, windows);
    REQUIRE(matched.has_value());
    CHECK(*matched == theta);
  }

  SUBCASE("scattered votes never lock") {
    constexpr std::int64_t theta = 123 * kSecond;
    const std::vector<subtitler::WhisperWindow> windows = {
        WindowAt(cues, 10, 12, theta),
        WindowAt(cues, 40, 42, theta + 10 * kSecond),
        WindowAt(cues, 70, 72, theta + 20 * kSecond),
    };

    CHECK_FALSE(subtitler::MatchTranscript(cues, windows).has_value());
  }

  SUBCASE("a repeated common phrase does not lock") {
    // The phrase "i love you" ends every cue of this document.
    std::vector<subtitler::SrtCue> repetitive;
    for (int i = 0; i < 100; ++i) {
      repetitive.push_back(subtitler::SrtCue{
          .start_ms = i * 3000,
          .end_ms = i * 3000 + 2500,
          .text =
              std::format("{} {} i love you", words[i * 2], words[i * 2 + 1])});
    }

    const std::vector<subtitler::WhisperWindow> phrase_only = {
        {"i love you i love you i love you", kSecond},
        {"i love you i love you", 2 * kSecond},
        {"i love you i love you i love you", 3 * kSecond},
    };
    CHECK_FALSE(
        subtitler::MatchTranscript(repetitive, phrase_only).has_value());

    // But the surrounding unique words still locate the region.
    constexpr std::int64_t theta = 60 * kSecond;
    const std::vector<subtitler::WhisperWindow> windows = {
        WindowAt(repetitive, 10, 12, theta),
        WindowAt(repetitive, 40, 42, theta),
        WindowAt(repetitive, 70, 72, theta),
    };
    const auto matched = subtitler::MatchTranscript(repetitive, windows);
    REQUIRE(matched.has_value());
    CHECK(*matched == theta);
  }

  SUBCASE("normalization makes case and punctuation irrelevant") {
    constexpr std::int64_t theta = 123 * kSecond;
    std::vector<subtitler::WhisperWindow> windows;
    for (const auto [first, last] :
         {std::pair{10UL, 12UL}, {40UL, 42UL}, {70UL, 72UL}}) {
      auto window = WindowAt(cues, first, last, theta);
      for (auto& c : window.text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      window.text += "...";
      windows.push_back(window);
    }

    CHECK(subtitler::MatchTranscript(cues, windows).has_value());
  }
}
