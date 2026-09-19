#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "utils/file_replacement.h"

namespace {
struct TempDirectory {
  TempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "subtitler-replacement-test-XXXXXX").string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    path = pattern;
  }
  ~TempDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

std::string Read(const std::filesystem::path& path) {
  std::ifstream file{path};
  return {std::istreambuf_iterator<char>{file}, {}};
}
}  // namespace

TEST_CASE("failed replacement restores bytes before playback rollback") {
  TempDirectory dir;
  const auto target = dir.path / "Movie.srt";
  std::ofstream{target} << "previous";
  bool replayed_previous = false;
  CHECK_FALSE(subtitler::ReplaceAndActivateFile(
      target, "candidate", [&](const auto& path, const auto& restore) {
        CHECK(Read(path) == "candidate");
        // The same callback Stream invokes before reconstructing the
        // old pipeline. Reopening this path must read the old bytes.
        REQUIRE(restore());
        replayed_previous = Read(path) == "previous";
        return false;
      }));
  CHECK(replayed_previous);
  CHECK(Read(target) == "previous");
  CHECK(std::distance(std::filesystem::directory_iterator{dir.path},
                      std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("early activation failure removes a new uncommitted file") {
  TempDirectory dir;
  const auto target = dir.path / "New.srt";
  CHECK_FALSE(subtitler::ReplaceAndActivateFile(
      target, "candidate", [](const auto&, const auto&) { return false; }));
  CHECK_FALSE(std::filesystem::exists(target));
  CHECK(std::filesystem::is_empty(dir.path));
}

TEST_CASE("committed replacement keeps new bytes and removes backup") {
  TempDirectory dir;
  // Staging must not append a suffix to the 255-byte destination name.
  const auto target = dir.path / (std::string(251, 'x') + ".srt");
  std::ofstream{target} << "previous";
  CHECK(subtitler::ReplaceAndActivateFile(
      target, "candidate", [](const auto&, const auto&) { return true; }));
  CHECK(Read(target) == "candidate");
  CHECK(std::distance(std::filesystem::directory_iterator{dir.path},
                      std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("an activation exception preserves the previous file") {
  TempDirectory dir;
  const auto target = dir.path / "Movie.srt";
  std::ofstream{target} << "previous";
  CHECK_THROWS_AS(subtitler::ReplaceAndActivateFile(
      target, "candidate", [](const auto&, const auto&) -> bool {
        throw std::runtime_error{"injected activation failure"};
      }), std::runtime_error);
  CHECK(Read(target) == "previous");
}

TEST_CASE("staging failure never reaches activation") {
  TempDirectory dir;
  const auto target = dir.path / "Movie.srt";
  std::filesystem::create_directory(target);
  bool called = false;
  CHECK_FALSE(subtitler::ReplaceAndActivateFile(
      target, "candidate", [&](const auto&, const auto&) {
        called = true;
        return true;
      }));
  CHECK_FALSE(called);
  CHECK(std::filesystem::is_directory(target));
}
