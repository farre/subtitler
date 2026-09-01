#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "utils/paths.h"

namespace {

// Saves and restores the environment variables the path resolution
// functions read, so the test's manipulations don't leak into other
// cases.
struct EnvGuard {
  EnvGuard() {
    if (const char* value = std::getenv("XDG_STATE_HOME")) {
      xdg_state_home = value;
    }
    if (const char* value = std::getenv("XDG_CONFIG_HOME")) {
      xdg_config_home = value;
    }
    if (const char* value = std::getenv("XDG_DATA_HOME")) {
      xdg_data_home = value;
    }
    if (const char* value = std::getenv("XDG_DATA_DIRS")) {
      xdg_data_dirs = value;
    }
    if (const char* value = std::getenv("HOME")) {
      home = value;
    }
  }

  ~EnvGuard() {
    Restore("XDG_STATE_HOME", xdg_state_home);
    Restore("XDG_CONFIG_HOME", xdg_config_home);
    Restore("XDG_DATA_HOME", xdg_data_home);
    Restore("XDG_DATA_DIRS", xdg_data_dirs);
    Restore("HOME", home);
  }

  static void Restore(const char* name,
                      const std::optional<std::string>& value) {
    if (value) {
      setenv(name, value->c_str(), 1);
    } else {
      unsetenv(name);
    }
  }

  std::optional<std::string> xdg_state_home;
  std::optional<std::string> xdg_config_home;
  std::optional<std::string> xdg_data_home;
  std::optional<std::string> xdg_data_dirs;
  std::optional<std::string> home;
};

}  // namespace

TEST_CASE("state directory resolution") {
  const EnvGuard guard;

  SUBCASE("XDG_STATE_HOME wins") {
    setenv("XDG_STATE_HOME", "/tmp/xdg-state", 1);
    setenv("HOME", "/tmp/home", 1);
    CHECK(subtitler::StateDirectory() ==
          std::filesystem::path{"/tmp/xdg-state/subtitler"});
  }

  SUBCASE("falls back to ~/.local/state") {
    unsetenv("XDG_STATE_HOME");
    setenv("HOME", "/tmp/home", 1);
    CHECK(subtitler::StateDirectory() ==
          std::filesystem::path{"/tmp/home/.local/state/subtitler"});
  }

  SUBCASE("unresolvable without either variable") {
    unsetenv("XDG_STATE_HOME");
    unsetenv("HOME");
    CHECK(subtitler::StateDirectory() == std::nullopt);
  }
}

TEST_CASE("config directory resolution") {
  const EnvGuard guard;

  SUBCASE("XDG_CONFIG_HOME wins") {
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-config", 1);
    setenv("HOME", "/tmp/home", 1);
    CHECK(subtitler::ConfigDirectory() ==
          std::filesystem::path{"/tmp/xdg-config/subtitler"});
  }

  SUBCASE("falls back to ~/.config") {
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/tmp/home", 1);
    CHECK(subtitler::ConfigDirectory() ==
          std::filesystem::path{"/tmp/home/.config/subtitler"});
  }

  SUBCASE("unresolvable without either variable") {
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
    CHECK(subtitler::ConfigDirectory() == std::nullopt);
  }
}

TEST_CASE("active subtitle file resolution") {
  const auto root =
      std::filesystem::temp_directory_path() / "subtitler-paths-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "subtitles");

  SUBCASE("no active marker") {
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker names a missing file") {
    {
      std::ofstream{root / "active"} << "movie.srt\n";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker names an existing library file") {
    {
      std::ofstream{root / "active"} << "movie.srt\n";
    }
    {
      std::ofstream{root / "subtitles" / "movie.srt"} << "1\n";
    }
    const auto resolved = subtitler::ActiveSubtitleFile(root);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == root / "subtitles" / "movie.srt");
  }

  SUBCASE("marker is not a plain filename") {
    {
      std::ofstream{root / "active"} << "../elsewhere.srt\n";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker with a nested traversal") {
    {
      std::ofstream{root / "active"} << "m/../../elsewhere.srt\n";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker is an absolute path") {
    {
      std::ofstream{root / "active"} << "/etc/passwd\n";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker contains a backslash") {
    {
      std::ofstream{root / "active"} << "m\\movie.srt\n";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker names a directory") {
    std::filesystem::create_directories(root / "subtitles" / "m");
    {
      std::ofstream{root / "active"} << "m\n";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  SUBCASE("marker names a sharded library file") {
    std::filesystem::create_directories(root / "subtitles" / "m");
    {
      std::ofstream{root / "subtitles" / "m" / "movie.srt"} << "1\n";
    }
    {
      std::ofstream{root / "active"} << "m/movie.srt\n";
    }
    const auto resolved = subtitler::ActiveSubtitleFile(root);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == root / "subtitles" / "m" / "movie.srt");
  }

  SUBCASE("empty marker") {
    {
      std::ofstream{root / "active"} << "";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  std::filesystem::remove_all(root);
}

TEST_CASE("subtitle library path sharding") {
  SUBCASE("letter buckets are lowercase") {
    CHECK(subtitler::LibrarySubtitlePath("movie.srt") ==
          std::filesystem::path{"m"} / "movie.srt");
    CHECK(subtitler::LibrarySubtitlePath("Movie.srt") ==
          std::filesystem::path{"m"} / "Movie.srt");
    CHECK(subtitler::LibrarySubtitlePath("Zodiac.srt") ==
          std::filesystem::path{"z"} / "Zodiac.srt");
  }

  SUBCASE("the extension match is case-insensitive") {
    CHECK(subtitler::LibrarySubtitlePath("MOVIE.SRT") ==
          std::filesystem::path{"m"} / "MOVIE.SRT");
    CHECK(subtitler::LibrarySubtitlePath("movie.Srt") ==
          std::filesystem::path{"m"} / "movie.Srt");
  }

  SUBCASE("non-letter titles go in the underscore bucket") {
    CHECK(subtitler::LibrarySubtitlePath("3 Idiots.srt") ==
          std::filesystem::path{"_"} / "3 Idiots.srt");
    CHECK(subtitler::LibrarySubtitlePath("_private.srt") ==
          std::filesystem::path{"_"} / "_private.srt");
    CHECK(subtitler::LibrarySubtitlePath("über.srt") ==
          std::filesystem::path{"_"} / "über.srt");
  }

  SUBCASE("unusable titles are rejected") {
    CHECK(subtitler::LibrarySubtitlePath("") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath("movie") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath("movie.txt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath(".srt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath(".hidden.srt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath("dir/movie.srt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath("dir\\movie.srt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath("../movie.srt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitlePath("movie.srt/") == std::nullopt);
  }
}

TEST_CASE("library title derivation from a stored path") {
  const std::filesystem::path state{"/state"};

  SUBCASE("sharded and legacy flat entries yield their titles") {
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / "m" / "Movie.srt") == "Movie.srt");
    CHECK(subtitler::LibrarySubtitleTitle(state,
                                          state / "subtitles" / "Movie.srt") ==
          "Movie.srt");
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / "_" / "3 Idiots.srt") ==
          "3 Idiots.srt");
  }

  SUBCASE("paths outside the library are rejected") {
    CHECK(subtitler::LibrarySubtitleTitle(state, "/elsewhere/Movie.srt") ==
          std::nullopt);
    CHECK(subtitler::LibrarySubtitleTitle(state, state / "Movie.srt") ==
          std::nullopt);
    CHECK(subtitler::LibrarySubtitleTitle(state, "subtitles/m/Movie.srt") ==
          std::nullopt);
  }

  SUBCASE("non-canonical nesting is rejected") {
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / "x" / "Movie.srt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / "m" / "deep" / "Movie.srt") ==
          std::nullopt);
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / ".." / "subtitles" / "m" /
                           "Movie.srt") == std::nullopt);
  }

  SUBCASE("unusable titles are rejected") {
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / "m" / "Movie.txt") == std::nullopt);
    CHECK(subtitler::LibrarySubtitleTitle(
              state, state / "subtitles" / "m" / ".hidden.srt") ==
          std::nullopt);
    CHECK(subtitler::LibrarySubtitleTitle(state, state / "subtitles") ==
          std::nullopt);
  }
}

TEST_CASE("subtitle storage") {
  const auto root =
      std::filesystem::temp_directory_path() / "subtitler-store-test";
  std::filesystem::remove_all(root);

  SUBCASE("stores the file sharded and marks it active") {
    const auto stored = subtitler::StoreSubtitle(root, "Movie.srt",
                                                 "1\n00:00:01,000 --> "
                                                 "00:00:02,000\nHi\n");
    REQUIRE(stored.has_value());
    CHECK(*stored == root / "subtitles" / "m" / "Movie.srt");

    std::ifstream file{*stored, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{file},
                               std::istreambuf_iterator<char>{}};
    CHECK(contents == "1\n00:00:01,000 --> 00:00:02,000\nHi\n");

    const auto active = subtitler::ActiveSubtitleFile(root);
    REQUIRE(active.has_value());
    CHECK(*active == *stored);
  }

  SUBCASE("overwrites an existing entry") {
    REQUIRE(subtitler::StoreSubtitle(root, "movie.srt", "old\n").has_value());
    const auto stored = subtitler::StoreSubtitle(root, "movie.srt", "new\n");
    REQUIRE(stored.has_value());

    std::ifstream file{*stored};
    std::string contents;
    std::getline(file, contents);
    CHECK(contents == "new");
  }

  SUBCASE("rejects an invalid title without touching the state dir") {
    CHECK(subtitler::StoreSubtitle(root, "../evil.srt", "x\n") == std::nullopt);
    CHECK_FALSE(std::filesystem::exists(root / "subtitles"));
  }

  std::filesystem::remove_all(root);
}

TEST_CASE("subtitle library listing and lookup") {
  const auto root =
      std::filesystem::temp_directory_path() / "subtitler-library-test";
  std::filesystem::remove_all(root);

  SUBCASE("an empty or missing library lists nothing") {
    CHECK(subtitler::ListSubtitles(root).empty());

    std::filesystem::create_directories(root / "subtitles");
    CHECK(subtitler::ListSubtitles(root).empty());
  }

  SUBCASE("lists sharded and legacy flat entries, sorted") {
    REQUIRE(subtitler::StoreSubtitle(root, "Movie.srt", "x\n").has_value());
    REQUIRE(subtitler::StoreSubtitle(root, "3 Idiots.srt", "x\n").has_value());
    REQUIRE(subtitler::StoreSubtitle(root, "apple.srt", "x\n").has_value());
    // A legacy flat entry and a file that isn't a usable title.
    std::ofstream{root / "subtitles" / "legacy.srt"} << "x\n";
    std::ofstream{root / "subtitles" / "notes.txt"} << "x\n";

    CHECK(subtitler::ListSubtitles(root) ==
          std::vector<std::string>{"3 Idiots.srt", "Movie.srt", "apple.srt",
                                   "legacy.srt"});
  }

  SUBCASE("lookup finds sharded and legacy flat entries") {
    REQUIRE(subtitler::StoreSubtitle(root, "Movie.srt", "x\n").has_value());
    std::ofstream{root / "subtitles" / "legacy.srt"} << "x\n";

    CHECK(subtitler::FindLibrarySubtitle(root, "Movie.srt") ==
          std::filesystem::path{"m"} / "Movie.srt");
    CHECK(subtitler::FindLibrarySubtitle(root, "legacy.srt") ==
          std::filesystem::path{"legacy.srt"});
  }

  SUBCASE("lookup rejects unusable or missing titles") {
    CHECK(subtitler::FindLibrarySubtitle(root, "../evil.srt") == std::nullopt);
    CHECK(subtitler::FindLibrarySubtitle(root, "no-extension") == std::nullopt);
    CHECK(subtitler::FindLibrarySubtitle(root, "missing.srt") == std::nullopt);
  }

  SUBCASE("loading reads sharded and legacy flat entries") {
    REQUIRE(
        subtitler::StoreSubtitle(root, "Movie.srt", "1\ncue\n").has_value());
    std::ofstream{root / "subtitles" / "legacy.srt"} << "2\ncue\n";

    CHECK(subtitler::LoadLibrarySubtitle(root, "Movie.srt") == "1\ncue\n");
    CHECK(subtitler::LoadLibrarySubtitle(root, "legacy.srt") == "2\ncue\n");
  }

  SUBCASE("loading rejects unusable or missing titles") {
    CHECK(subtitler::LoadLibrarySubtitle(root, "../evil.srt") == std::nullopt);
    CHECK(subtitler::LoadLibrarySubtitle(root, "no-extension") == std::nullopt);
    CHECK(subtitler::LoadLibrarySubtitle(root, "missing.srt") == std::nullopt);
  }

  SUBCASE("removal deletes sharded and legacy flat entries") {
    REQUIRE(subtitler::StoreSubtitle(root, "Movie.srt", "x\n").has_value());
    std::ofstream{root / "subtitles" / "legacy.srt"} << "x\n";

    CHECK(subtitler::RemoveLibrarySubtitle(root, "Movie.srt"));
    CHECK(subtitler::FindLibrarySubtitle(root, "Movie.srt") == std::nullopt);

    CHECK(subtitler::RemoveLibrarySubtitle(root, "legacy.srt"));
    CHECK(subtitler::FindLibrarySubtitle(root, "legacy.srt") == std::nullopt);
  }

  SUBCASE("removal rejects unusable or missing titles") {
    CHECK_FALSE(subtitler::RemoveLibrarySubtitle(root, "../evil.srt"));
    CHECK_FALSE(subtitler::RemoveLibrarySubtitle(root, "no-extension"));
    CHECK_FALSE(subtitler::RemoveLibrarySubtitle(root, "missing.srt"));
  }

  SUBCASE("the active marker is set and cleared") {
    REQUIRE(subtitler::StoreSubtitle(root, "Movie.srt", "x\n").has_value());
    REQUIRE(subtitler::SetActiveSubtitle(root, "m/Movie.srt"));
    CHECK(subtitler::ActiveSubtitleFile(root) ==
          root / "subtitles" / "m" / "Movie.srt");

    subtitler::ClearActiveSubtitle(root);
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
    // Clearing with no marker is fine.
    subtitler::ClearActiveSubtitle(root);
  }

  std::filesystem::remove_all(root);
}

TEST_CASE("web root directory resolution") {
  const EnvGuard guard;

  const auto root =
      std::filesystem::temp_directory_path() / "subtitler-webroot-test";
  std::filesystem::remove_all(root);

  const auto home_data = root / "home-data";
  const auto system_data = root / "system-data";
  const auto other_data = root / "other-data";

  SUBCASE("unresolvable when nothing provides a web directory") {
    setenv("XDG_DATA_HOME", home_data.c_str(), 1);
    setenv("XDG_DATA_DIRS", system_data.c_str(), 1);
    CHECK(subtitler::WebRootDirectory() == std::nullopt);
  }

  SUBCASE("XDG_DATA_HOME wins") {
    std::filesystem::create_directories(home_data / "subtitler" / "web");
    std::filesystem::create_directories(system_data / "subtitler" / "web");
    setenv("XDG_DATA_HOME", home_data.c_str(), 1);
    setenv("XDG_DATA_DIRS", system_data.c_str(), 1);
    CHECK(subtitler::WebRootDirectory() == home_data / "subtitler" / "web");
  }

  SUBCASE("falls back to the first XDG_DATA_DIRS entry that has one") {
    std::filesystem::create_directories(other_data / "subtitler" / "web");
    setenv("XDG_DATA_HOME", home_data.c_str(), 1);
    const std::string dirs = system_data.string() + ":" + other_data.string();
    setenv("XDG_DATA_DIRS", dirs.c_str(), 1);
    CHECK(subtitler::WebRootDirectory() == other_data / "subtitler" / "web");
  }

  SUBCASE("empty XDG_DATA_DIRS entries are skipped") {
    std::filesystem::create_directories(system_data / "subtitler" / "web");
    unsetenv("XDG_DATA_HOME");
    unsetenv("HOME");
    const std::string dirs = ":" + system_data.string();
    setenv("XDG_DATA_DIRS", dirs.c_str(), 1);
    CHECK(subtitler::WebRootDirectory() == system_data / "subtitler" / "web");
  }

  SUBCASE("falls back to ~/.local/share when XDG_DATA_HOME is unset") {
    const auto home = root / "home";
    std::filesystem::create_directories(home / ".local" / "share" /
                                        "subtitler" / "web");
    unsetenv("XDG_DATA_HOME");
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_DATA_DIRS", system_data.c_str(), 1);
    CHECK(subtitler::WebRootDirectory() ==
          home / ".local" / "share" / "subtitler" / "web");
  }

  std::filesystem::remove_all(root);
}

TEST_CASE("whisper model store") {
  const auto root = std::filesystem::temp_directory_path() /
                    "subtitler-paths-test-whisper-models";

  SUBCASE("model name validation") {
    CHECK(subtitler::WhisperModelNameValid("ggml-tiny.en.bin"));
    CHECK(subtitler::WhisperModelNameValid("model.bin"));
    CHECK_FALSE(subtitler::WhisperModelNameValid(""));
    CHECK_FALSE(subtitler::WhisperModelNameValid(".bin"));
    CHECK_FALSE(subtitler::WhisperModelNameValid(".hidden.bin"));
    CHECK_FALSE(subtitler::WhisperModelNameValid("a/b.bin"));
    CHECK_FALSE(subtitler::WhisperModelNameValid("a\\b.bin"));
    CHECK_FALSE(subtitler::WhisperModelNameValid("model"));
    CHECK_FALSE(subtitler::WhisperModelNameValid("model.binx"));
  }

  SUBCASE("model path resolution") {
    CHECK(subtitler::WhisperModelPath(root, "ggml-tiny.en.bin") ==
          root / "models" / "ggml-tiny.en.bin");
    CHECK_FALSE(subtitler::WhisperModelPath(root, "../evil.bin").has_value());
  }

  SUBCASE("lists the stored models") {
    std::filesystem::create_directories(root / "models" / "odd.bin");
    {
      std::ofstream{root / "models" / "ggml-b.bin"};
      std::ofstream{root / "models" / "ggml-a.bin"};
      std::ofstream{root / "models" / "notes.txt"};
      std::ofstream{root / "models" / ".hidden.bin"};
    }

    CHECK(subtitler::ListWhisperModels(root) ==
          std::vector<std::string>{"ggml-a.bin", "ggml-b.bin"});

    std::filesystem::remove_all(root);
  }
}
