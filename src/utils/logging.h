#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace subtitler {

enum class LogLevel : std::uint8_t { kError, kWarning, kInfo, kDebug };

inline std::string_view LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kError:
      return "error";
    case LogLevel::kWarning:
      return "warn";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kDebug:
      return "debug";
  }
  return "unknown";
}

inline std::optional<LogLevel> ParseLogLevel(std::string_view name) {
  if (name == "error") {
    return LogLevel::kError;
  }
  if (name == "warn") {
    return LogLevel::kWarning;
  }
  if (name == "info") {
    return LogLevel::kInfo;
  }
  if (name == "debug") {
    return LogLevel::kDebug;
  }
  return std::nullopt;
}

// Label -> highest enabled level.
using LogConfig = std::map<std::string, LogLevel, std::less<>>;

// The catch-all label: enables every label without its own entry.
constexpr std::string_view kAllLabel = "all";

// Parses a SUBTITLER_LOG spec: a comma-separated list of <label>:<level>
// entries (e.g. "mjpeg:info,stream:debug"). Malformed entries and unknown
// levels are silently ignored; a repeated label keeps the last entry.
inline LogConfig ParseLogConfig(std::string_view spec) {
  LogConfig config;

  for (const auto entry : std::views::split(spec, ',')) {
    const std::string_view token{entry};
    const auto colon = token.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        token.find(':', colon + 1) != std::string_view::npos) {
      continue;
    }

    const auto level = ParseLogLevel(token.substr(colon + 1));
    if (!level) {
      continue;
    }

    config[std::string{token.substr(0, colon)}] = *level;
  }

  return config;
}

// A label's own entry wins over the catch-all, regardless of direction:
// "all:debug,stream:error" logs stream at error only.
inline bool LogEnabled(const LogConfig& config, std::string_view name,
                       LogLevel level) {
  auto it = config.find(name);
  if (it == config.end()) {
    it = config.find(kAllLabel);
  }
  return it != config.end() &&
         std::to_underlying(level) <= std::to_underlying(it->second);
}

// The process-wide config, parsed once from SUBTITLER_LOG.
inline const LogConfig& ActiveLogConfig() {
  static const LogConfig config = [] {
    const char* spec = std::getenv("SUBTITLER_LOG");
    return ParseLogConfig(spec != nullptr ? std::string_view{spec}
                                          : std::string_view{});
  }();
  return config;
}

inline bool LogEnabled(std::string_view name, LogLevel level) {
  return LogEnabled(ActiveLogConfig(), name, level);
}

inline void LogMessage(std::string_view name, LogLevel level,
                       const std::string& message) {
  const auto now = std::chrono::floor<std::chrono::milliseconds>(
      std::chrono::system_clock::now());
  std::println(stderr, "{:%F %T}Z [{}] [{}] {}", now, LogLevelName(level), name,
               message);
}

}  // namespace subtitler

#define SUBTITLER_LOG(name, level, ...)                               \
  do {                                                                \
    if (::subtitler::LogEnabled(name, level)) {                       \
      ::subtitler::LogMessage(name, level, std::format(__VA_ARGS__)); \
    }                                                                 \
  } while (0)

// Label-specific macros, one per directory, for all compilation units.
#define CONFIG_LOG(level, ...) SUBTITLER_LOG("config", level, __VA_ARGS__)
#define MAIN_LOG(level, ...) SUBTITLER_LOG("main", level, __VA_ARGS__)
#define STREAM_LOG(level, ...) SUBTITLER_LOG("stream", level, __VA_ARGS__)
#define SYNC_LOG(level, ...) SUBTITLER_LOG("sync", level, __VA_ARGS__)
#define WEB_LOG(level, ...) SUBTITLER_LOG("web", level, __VA_ARGS__)
