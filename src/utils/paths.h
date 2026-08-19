#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace subtitler {

// $XDG_STATE_HOME/subtitler, or ~/.local/state/subtitler when the XDG
// variable is unset. nullopt when neither variable gives a usable root.
inline std::optional<std::filesystem::path> StateDirectory() {
  if (const char* xdg = std::getenv("XDG_STATE_HOME");
      xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path{xdg} / "subtitler";
  }

  if (const char* home = std::getenv("HOME");
      home != nullptr && *home != '\0') {
    return std::filesystem::path{home} / ".local" / "state" / "subtitler";
  }

  return std::nullopt;
}

// The subtitle file selected for boot resume (#438): <state_dir>/active
// names a file inside <state_dir>/subtitles/. nullopt when nothing is
// selected or the named file is gone.
inline std::optional<std::filesystem::path> ActiveSubtitleFile(
    const std::filesystem::path& state_dir) {
  std::string name;
  if (std::ifstream active{state_dir / "active"};
      !std::getline(active, name)) {
    return std::nullopt;
  }

  // The active marker names a library entry, nothing else.
  if (name.empty() || name.find('/') != std::string::npos) {
    return std::nullopt;
  }

  const auto path = state_dir / "subtitles" / name;
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  return path;
}

}  // namespace subtitler
