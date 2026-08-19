#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

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

// The directory static web assets are served from: the first existing
// <dir>/subtitler/web across $XDG_DATA_HOME and $XDG_DATA_DIRS
// (defaulting to ~/.local/share and /usr/local/share:/usr/share).
// nullopt when no installation provides one.
inline std::optional<std::filesystem::path> WebRootDirectory() {
  const auto in = [](const std::filesystem::path& base)
      -> std::optional<std::filesystem::path> {
    const auto root = base / "subtitler" / "web";
    if (std::filesystem::is_directory(root)) {
      return root;
    }
    return std::nullopt;
  };

  if (const char* data_home = std::getenv("XDG_DATA_HOME");
      data_home != nullptr && *data_home != '\0') {
    if (auto root = in(data_home)) {
      return root;
    }
  } else if (const char* home = std::getenv("HOME");
             home != nullptr && *home != '\0') {
    if (auto root = in(std::filesystem::path{home} / ".local" / "share")) {
      return root;
    }
  }

  const char* dirs = std::getenv("XDG_DATA_DIRS");
  const std::string_view list =
      (dirs != nullptr && *dirs != '\0')
          ? std::string_view{dirs}
          : std::string_view{"/usr/local/share:/usr/share"};

  for (const auto dir : std::views::split(list, ':')) {
    const std::string_view base{dir};
    if (base.empty()) {
      continue;
    }
    if (auto root = in(base)) {
      return root;
    }
  }

  return std::nullopt;
}

// The library-relative path (<bucket>/<title>) for a stored subtitle
// (#212). The bucket is the title's first letter lowercased, or "_"
// for titles not starting with an ASCII letter. nullopt for titles
// that aren't usable library names: empty, dot-relative, containing
// slashes, or not ending in .srt.
inline std::optional<std::filesystem::path> LibrarySubtitlePath(
    std::string_view title) {
  constexpr std::string_view kExtension = ".srt";

  if (title.size() <= kExtension.size() || title.front() == '.' ||
      title.find_first_of("/\\") != std::string_view::npos) {
    return std::nullopt;
  }

  if (!std::ranges::equal(title.substr(title.size() - kExtension.size()),
                          kExtension, [](char a, char b) {
                            return std::tolower(
                                       static_cast<unsigned char>(a)) ==
                                   std::tolower(static_cast<unsigned char>(b));
                          })) {
    return std::nullopt;
  }

  const auto first = static_cast<unsigned char>(title.front());
  const char bucket =
      std::isalpha(first) != 0 ? static_cast<char>(std::tolower(first)) : '_';

  return std::filesystem::path{std::string{bucket}} / title;
}

// Stores contents as the library entry for title (sharded by
// LibrarySubtitlePath), marks it active for boot resume, and returns
// the full path. nullopt on an invalid title or any I/O failure.
inline std::optional<std::filesystem::path> StoreSubtitle(
    const std::filesystem::path& state_dir, std::string_view title,
    std::string_view contents) {
  const auto relative = LibrarySubtitlePath(title);
  if (!relative) {
    return std::nullopt;
  }

  const auto path = state_dir / "subtitles" / *relative;

  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return std::nullopt;
  }

  if (std::ofstream file{path, std::ios::binary | std::ios::trunc};
      !(file << contents)) {
    return std::nullopt;
  }

  // generic_string: the marker always uses '/' as the separator.
  if (std::ofstream active{state_dir / "active", std::ios::trunc};
      !(active << relative->generic_string() << '\n')) {
    return std::nullopt;
  }

  return path;
}

// The subtitle file selected for boot resume (#438): <state_dir>/active
// names a file inside <state_dir>/subtitles/ — a sharded <bucket>/<name>
// entry (#212) or a legacy flat <name>. nullopt when nothing is
// selected or the named file is gone.
inline std::optional<std::filesystem::path> ActiveSubtitleFile(
    const std::filesystem::path& state_dir) {
  std::string name;
  if (std::ifstream active{state_dir / "active"}; !std::getline(active, name)) {
    return std::nullopt;
  }

  // The active marker names a library entry, nothing else: a relative
  // path inside subtitles/ with no traversal components.
  const std::filesystem::path relative{name};
  if (name.empty() || name.find('\\') != std::string::npos ||
      relative.is_absolute()) {
    return std::nullopt;
  }

  for (const auto& component : relative) {
    if (component == "..") {
      return std::nullopt;
    }
  }

  const auto path = state_dir / "subtitles" / relative;
  if (!std::filesystem::is_regular_file(path)) {
    return std::nullopt;
  }

  return path;
}

}  // namespace subtitler
