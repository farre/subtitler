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
#include <vector>

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

// $XDG_CONFIG_HOME/subtitler, or ~/.config/subtitler when the XDG
// variable is unset. nullopt when neither variable gives a usable root.
inline std::optional<std::filesystem::path> ConfigDirectory() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path{xdg} / "subtitler";
  }

  if (const char* home = std::getenv("HOME");
      home != nullptr && *home != '\0') {
    return std::filesystem::path{home} / ".config" / "subtitler";
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

// Marks the library-relative entry (<bucket>/<title> or a legacy flat
// <title>) active for boot resume. false on I/O failure.
inline bool SetActiveSubtitle(const std::filesystem::path& state_dir,
                              std::string_view relative) {
  if (std::ofstream active{state_dir / "active", std::ios::trunc};
      !(active << relative << '\n')) {
    return false;
  }

  return true;
}

// Clears the active marker, so the next boot attaches no subtitles.
inline void ClearActiveSubtitle(const std::filesystem::path& state_dir) {
  std::error_code error;
  std::filesystem::remove(state_dir / "active", error);
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
  if (!SetActiveSubtitle(state_dir, relative->generic_string())) {
    return std::nullopt;
  }

  return path;
}

// The library entry for a title: the sharded subtitles/<bucket>/<title>,
// or a legacy flat subtitles/<title>. Returns the library-relative path;
// nullopt for unusable titles or when no entry exists.
inline std::optional<std::filesystem::path> FindLibrarySubtitle(
    const std::filesystem::path& state_dir, std::string_view title) {
  auto relative = LibrarySubtitlePath(title);
  if (!relative) {
    return std::nullopt;
  }

  const auto root = state_dir / "subtitles";
  if (std::filesystem::is_regular_file(root / *relative)) {
    return relative;
  }

  const std::filesystem::path flat{title};
  if (std::filesystem::is_regular_file(root / flat)) {
    return flat;
  }

  return std::nullopt;
}

// The library title for a full path naming a library entry: the
// sharded subtitles/<bucket>/<title> or a legacy flat subtitles/<title>.
// nullopt for paths outside the library or unusable titles. Lexical
// only — checks neither existence nor that the entry is stored there.
inline std::optional<std::string> LibrarySubtitleTitle(
    const std::filesystem::path& state_dir,
    const std::filesystem::path& path) {
  const auto relative = path.lexically_relative(state_dir / "subtitles");
  const auto title = relative.filename().string();
  const auto canonical = LibrarySubtitlePath(title);
  if (!canonical) {
    return std::nullopt;
  }

  // The sharded entry or a legacy flat one; anything else (deeper
  // nesting, a wrong bucket, traversal) is not a library title.
  if (relative != *canonical && relative != std::filesystem::path{title}) {
    return std::nullopt;
  }

  return title;
}

// The contents of a library entry (sharded or legacy flat); nullopt
// for unusable titles, missing entries, or unreadable files.
inline std::optional<std::string> LoadLibrarySubtitle(
    const std::filesystem::path& state_dir, std::string_view title) {
  const auto relative = FindLibrarySubtitle(state_dir, title);
  if (!relative) {
    return std::nullopt;
  }

  std::ifstream file{state_dir / "subtitles" / *relative, std::ios::binary};
  if (!file) {
    return std::nullopt;
  }

  std::string contents{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
  if (file.bad()) {
    return std::nullopt;
  }

  return contents;
}

// Removes the library entry for a title (sharded or legacy flat, #453).
// false when no entry exists or the removal fails.
inline bool RemoveLibrarySubtitle(const std::filesystem::path& state_dir,
                                  std::string_view title) {
  const auto relative = FindLibrarySubtitle(state_dir, title);
  if (!relative) {
    return false;
  }

  std::error_code error;
  return std::filesystem::remove(state_dir / "subtitles" / *relative, error) &&
         !error;
}

// The activatable library titles (those passing LibrarySubtitlePath),
// sharded and legacy flat entries, sorted. Scanned on demand (#441).
inline std::vector<std::string> ListSubtitles(
    const std::filesystem::path& state_dir) {
  std::vector<std::string> titles;

  const auto collect = [&titles](const std::filesystem::path& dir) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
      auto name = entry.path().filename().string();
      if (entry.is_regular_file(error) && LibrarySubtitlePath(name)) {
        titles.push_back(std::move(name));
      }
    }
  };

  const auto root = state_dir / "subtitles";
  collect(root);  // legacy flat entries

  std::error_code error;
  for (const auto& bucket : std::filesystem::directory_iterator(root, error)) {
    if (bucket.is_directory(error)) {
      collect(bucket.path());
    }
  }

  std::ranges::sort(titles);
  return titles;
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

// The whisper model store (#19): <state_dir>/models, holding ggml
// *.bin files selected and downloaded through the web API.
inline std::filesystem::path WhisperModelsDirectory(
    const std::filesystem::path& state_dir) {
  return state_dir / "models";
}

// A usable model file name: non-empty, doesn't start with a dot,
// contains no slashes, ends in .bin (e.g. ggml-tiny.en.bin).
inline bool WhisperModelNameValid(std::string_view name) {
  constexpr std::string_view kExtension = ".bin";

  return name.size() > kExtension.size() && name.front() != '.' &&
         name.find_first_of("/\\") == std::string_view::npos &&
         name.substr(name.size() - kExtension.size()) == kExtension;
}

// The store path for a model name, existing or not; nullopt for
// invalid names.
inline std::optional<std::filesystem::path> WhisperModelPath(
    const std::filesystem::path& state_dir, std::string_view name) {
  if (!WhisperModelNameValid(name)) {
    return std::nullopt;
  }

  return WhisperModelsDirectory(state_dir) / std::string{name};
}

// The stored model names, sorted. Scanned on demand.
inline std::vector<std::string> ListWhisperModels(
    const std::filesystem::path& state_dir) {
  std::vector<std::string> names;

  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(
           WhisperModelsDirectory(state_dir), error)) {
    auto name = entry.path().filename().string();
    if (entry.is_regular_file(error) && WhisperModelNameValid(name)) {
      names.push_back(std::move(name));
    }
  }

  std::ranges::sort(names);
  return names;
}

// Removes the model named name from the store; false when the name is
// invalid, no such model is stored, or the removal fails.
inline bool RemoveWhisperModel(const std::filesystem::path& state_dir,
                               std::string_view name) {
  const auto path = WhisperModelPath(state_dir, name);
  if (!path) {
    return false;
  }

  std::error_code error;
  return std::filesystem::remove(*path, error) && !error;
}

}  // namespace subtitler
