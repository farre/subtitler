#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "utils/paths.h"

namespace {

// Saves and restores the environment variables StateDirectory reads, so
// the test's manipulations don't leak into other cases.
struct EnvGuard {
  EnvGuard() {
    if (const char* value = std::getenv("XDG_STATE_HOME")) {
      xdg_state_home = value;
    }
    if (const char* value = std::getenv("HOME")) {
      home = value;
    }
  }

  ~EnvGuard() {
    Restore("XDG_STATE_HOME", xdg_state_home);
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

  SUBCASE("empty marker") {
    {
      std::ofstream{root / "active"} << "";
    }
    CHECK(subtitler::ActiveSubtitleFile(root) == std::nullopt);
  }

  std::filesystem::remove_all(root);
}
