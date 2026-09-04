// subtitler-test: the sync matcher workbench. Matches transcript
// fragments against an SRT with exactly the production sync module
// (src/sync), so matcher improvements developed here carry over to the
// appliance unchanged. Depends on subtitler::sync only.

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <istream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sync/srt_cues.h"
#include "sync/sync_matcher.h"
#include "sync/sync_session.h"

namespace {

constexpr std::string_view kUsage =
    "usage: subtitler-test <srt-file> [--windows=<file>] [<fragment>...]\n"
    "\n"
    "Matches transcript fragments against an SRT with the production sync\n"
    "matcher (src/sync). Each fragment is one whisper-style window with\n"
    "timestamp 0, so a vote's theta reads directly as the fragment's end\n"
    "position in the SRT. --windows reads additional windows from a file\n"
    "(\"-\" for stdin), one per line: \"<timestamp-ns>\\t<text>\", or bare\n"
    "\"<text>\" (timestamp 0); empty lines and '#' comments are skipped.\n"
    "\n"
    "Exit status: 0 when the windows lock, 1 when they don't, 2 on usage\n"
    "or file errors.";

std::optional<std::string> ReadStream(std::istream& stream) {
  if (!stream) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream file{path};
  return ReadStream(file);
}

// "<timestamp-ns>\t<text>", or bare "<text>" with timestamp 0. The tab
// form is what the web interface's capture log produces.
subtitler::TimestampedText ParseWindowLine(std::string_view line) {
  if (line.ends_with('\r')) {
    line.remove_suffix(1);
  }
  if (const auto tab = line.find('\t'); tab != std::string_view::npos) {
    std::int64_t timestamp_ns = 0;
    const auto prefix = line.substr(0, tab);
    const auto* end = prefix.data() + prefix.size();
    if (const auto parsed =
            std::from_chars(prefix.data(), end, timestamp_ns);
        parsed.ec == std::errc{} && parsed.ptr == end) {
      return {std::string{line.substr(tab + 1)}, timestamp_ns};
    }
  }
  return {std::string{line}, 0};
}

std::string FormatMs(std::int64_t ms) {
  const bool negative = ms < 0;
  const auto total = static_cast<std::uint64_t>(negative ? -ms : ms);
  return std::format("{}{:02}:{:02}:{:02}.{:03}", negative ? "-" : "",
                     total / 3'600'000, total / 60'000 % 60,
                     total / 1'000 % 60, total % 1'000);
}

std::string_view RejectReason(subtitler::WindowReject reject) {
  switch (reject) {
    case subtitler::WindowReject::kTooShort:
      return "too short";
    case subtitler::WindowReject::kBelowThreshold:
      return "below threshold";
    case subtitler::WindowReject::kAmbiguous:
      return "ambiguous";
    case subtitler::WindowReject::kOutOfRange:
      return "outside the SRT stream";
    case subtitler::WindowReject::kNone:
      break;
  }
  return "";
}

// The cue showing at position ms, if any.
std::optional<std::size_t> CueAt(const std::vector<subtitler::SrtCue>& cues,
                                 std::int64_t ms) {
  for (std::size_t i = 0; i < cues.size(); ++i) {
    if (cues[i].start_ms <= ms && ms < cues[i].end_ms) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args{argv + 1, argv + argc};
  if (args.empty() || args.front() == "--help" || args.front() == "-h") {
    std::println("{}", kUsage);
    return args.empty() ? 2 : 0;
  }

  std::vector<subtitler::TimestampedText> windows;
  for (std::size_t i = 1; i < args.size(); ++i) {
    constexpr std::string_view windows_flag = "--windows=";
    if (args[i].starts_with(windows_flag)) {
      const auto path = args[i].substr(windows_flag.size());
      const auto contents = path == "-" ? ReadStream(std::cin) : ReadFile(path);
      if (!contents) {
        std::println(stderr, "Could not read windows file: {}", path);
        return 2;
      }
      std::istringstream lines{*contents};
      for (std::string line; std::getline(lines, line);) {
        if (line.empty() || line.starts_with('#')) {
          continue;
        }
        windows.push_back(ParseWindowLine(line));
      }
    } else if (args[i].starts_with("--")) {
      std::println(stderr, "Unknown option: {}\n{}", args[i], kUsage);
      return 2;
    } else {
      windows.push_back({args[i], 0});
    }
  }

  if (windows.empty()) {
    std::println(stderr, "No transcript windows given.\n{}", kUsage);
    return 2;
  }

  const auto srt = ReadFile(args.front());
  if (!srt) {
    std::println(stderr, "Could not read SRT file: {}", args.front());
    return 2;
  }
  const auto cues = subtitler::ParseSrtCues(*srt);

  std::print("srt: {} cues", cues.size());
  if (!cues.empty()) {
    std::print(", span {} – {}", FormatMs(cues.front().start_ms),
               FormatMs(cues.back().end_ms));
  }
  std::println("\n");

  std::size_t voted = 0;
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const auto& window = windows[i];
    std::println("window {} (ts {:.3f} s): \"{}\"", i + 1,
                 window.timestamp_ns / 1e9, window.text);

    const auto words = subtitler::NormalizeMatchWords(window.text);
    std::print("  {} words:", words.size());
    for (const auto& word : words) {
      std::print(" {}", word);
    }
    std::println("");

    const auto match = subtitler::MatchWindow(cues, window);
    std::print("  best offset: {} hits (runner-up {})", match.best_hits,
               match.runner_up_hits);
    if (!match.theta_ns) {
      std::println(" — {}", RejectReason(match.reject));
      continue;
    }

    ++voted;
    const std::int64_t end_ms =
        (window.timestamp_ns + *match.theta_ns) / 1'000'000;
    std::print("\n  vote θ = {} ms → window ends at srt {}",
               *match.theta_ns / 1'000'000, FormatMs(end_ms));
    if (const auto cue = CueAt(cues, end_ms)) {
      std::print(" — cue #{}: \"{}\"", *cue + 1, cues[*cue].text);
    }
    std::println("");
  }

  std::println("");
  if (const auto theta = subtitler::MatchTranscript(cues, windows)) {
    std::println("match: locked, θ = {} ms ({} of {} windows voted)",
                 *theta / 1'000'000, voted, windows.size());
    return 0;
  }
  std::println("match: no lock ({} of {} windows voted)", voted,
               windows.size());
  return 1;
}
