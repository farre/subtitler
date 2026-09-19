#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/resource.h>

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "config/config.h"

namespace {

// A unique temporary directory per test case, removed on destruction.
struct TempDir {
  TempDir() {
    path = std::filesystem::temp_directory_path() / std::to_string(counter++) /
           "subtitler-config-test";
    std::filesystem::create_directories(path);
  }

  ~TempDir() { std::filesystem::remove_all(path); }

  std::filesystem::path File(std::string_view contents) const {
    const auto file = path / "config.ini";
    std::ofstream out{file, std::ios::binary | std::ios::trunc};
    out << contents;
    return file;
  }

  inline static int counter = 0;
  std::filesystem::path path;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

constexpr std::string_view kFullConfig = R"(
[subtitler]
version = 1

[capture]
device = /dev/video2
audio = false

[output]
mode = window
connector-id = 32
audio-device = plughw:CARD=vc4hdmi,DEV=0
audio-offset-ms = -150

[web]
enabled = true
root = /srv/www
api-key = s3cret

[subtitles]
file = /library/m/Movie.srt
visible = false
delay-ms = 250
font-family = DejaVu Sans
font-size-pt = 28
font-color = #12aB34

[whisper]
enabled = true
model = ggml-tiny.en.bin
)";

}  // namespace

TEST_CASE("missing file yields an empty writable config") {
  const TempDir dir;
  const auto config = subtitler::Config::Load(dir.path / "config.ini");

  REQUIRE(config != nullptr);
  CHECK(config->values().device == std::nullopt);
  CHECK(config->values().subtitle_file == std::nullopt);

  config->SetSubtitlesVisible(true);
  REQUIRE(config->Save());
  CHECK(std::filesystem::is_regular_file(dir.path / "config.ini"));
  CHECK(!std::filesystem::exists(dir.path / "config.ini.tmp"));
}

TEST_CASE("full config parses") {
  const TempDir dir;
  const auto config = subtitler::Config::Load(dir.File(kFullConfig));

  REQUIRE(config != nullptr);
  const auto& values = config->values();
  CHECK(values.device == "/dev/video2");
  CHECK(values.audio == false);
  CHECK(values.output_mode == "window");
  CHECK(values.connector_id == 32);
  CHECK(values.audio_output_device == "plughw:CARD=vc4hdmi,DEV=0");
  CHECK(values.audio_offset_ms == -150);
  CHECK(values.web == true);
  CHECK(values.web_root == "/srv/www");
  CHECK(values.api_key == "s3cret");
  CHECK(values.subtitle_file == "/library/m/Movie.srt");
  CHECK(values.subtitles_visible == false);
  CHECK(values.subtitle_delay_ms == 250);
  CHECK(values.subtitle_font_family == "DejaVu Sans");
  CHECK(values.subtitle_font_size_pt == 28);
  CHECK(values.subtitle_font_color == 0xFF12AB34u);
  CHECK(values.whisper_enabled == true);
  CHECK(values.whisper_model == "ggml-tiny.en.bin");
}

TEST_CASE("invalid values are dropped, valid ones kept") {
  const TempDir dir;
  const auto config = subtitler::Config::Load(dir.File(R"(
[output]
connector-id = soon
audio-offset-ms = -150

[web]
enabled = maybe

[subtitles]
font-color = 12ab34
font-color2 = #12ab34
font-size-pt = huge

[whisper]
enabled = maybe
model = ../evil.bin
)"));

  REQUIRE(config != nullptr);
  const auto& values = config->values();
  CHECK(values.connector_id == std::nullopt);
  CHECK(values.audio_offset_ms == -150);
  CHECK(values.web == std::nullopt);
  CHECK(values.subtitle_font_color == std::nullopt);
  CHECK(values.subtitle_font_size_pt == std::nullopt);
  CHECK(values.whisper_enabled == std::nullopt);
  CHECK(values.whisper_model == std::nullopt);
}

TEST_CASE("out-of-range numeric values are dropped (#446)") {
  const TempDir dir;
  const auto config = subtitler::Config::Load(dir.File(R"(
[subtitles]
delay-ms = 2305843009214
font-size-pt = 1001
visible = true
)"));

  REQUIRE(config != nullptr);
  const auto& values = config->values();
  CHECK(values.subtitle_delay_ms == std::nullopt);
  CHECK(values.subtitle_font_size_pt == std::nullopt);
  // Invalid keys must not invalidate unrelated good keys.
  CHECK(values.subtitles_visible == true);
}

TEST_CASE("boundary numeric values parse (#446)") {
  const TempDir dir;
  const auto config = subtitler::Config::Load(dir.File(R"(
[subtitles]
delay-ms = -2305843009213
font-size-pt = 1000
)"));

  REQUIRE(config != nullptr);
  CHECK(config->values().subtitle_delay_ms == -2305843009213LL);
  CHECK(config->values().subtitle_font_size_pt == 1000);
}

TEST_CASE("empty values count as unset") {
  const TempDir dir;
  const auto config = subtitler::Config::Load(dir.File(R"(
[capture]
device =

[web]
api-key =
)"));

  REQUIRE(config != nullptr);
  CHECK(config->values().device == std::nullopt);
  CHECK(config->values().api_key == std::nullopt);
}

TEST_CASE("unusable files are refused") {
  const TempDir dir;

  SUBCASE("malformed INI") {
    CHECK(subtitler::Config::Load(dir.File("not [ini\n")) == nullptr);
  }

  SUBCASE("unsupported version") {
    CHECK(subtitler::Config::Load(dir.File("[subtitler]\nversion = 2\n")) ==
          nullptr);
  }

  SUBCASE("unparseable version") {
    CHECK(subtitler::Config::Load(dir.File("[subtitler]\nversion = x\n")) ==
          nullptr);
  }
}

TEST_CASE("write-back round-trips and preserves the untouched content") {
  const TempDir dir;
  const auto path = dir.File(R"(
# leading comment
[web]
enabled = true
future-key = keep me

[subtitles]
file = /library/m/Old.srt
delay-ms = 10
)");

  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);

  config->SetSubtitleFile("/library/n/New.srt");
  config->SetSubtitlesVisible(false);
  config->SetSubtitleDelayMs(-40);
  config->SetSubtitleFontFamily("DejaVu Sans");
  config->SetSubtitleFontSizePt(30);
  config->SetSubtitleFontColor(0xFF112233);
  REQUIRE(config->Save());

  const std::string saved = ReadFile(path);
  // GKeyFile normalizes the key=value spacing but keeps comments and
  // unknown keys.
  CHECK(saved.find("# leading comment") != std::string::npos);
  CHECK(saved.find("future-key=keep me") != std::string::npos);

  const auto reloaded = subtitler::Config::Load(path);
  REQUIRE(reloaded != nullptr);
  const auto& values = reloaded->values();
  CHECK(values.web == true);
  CHECK(values.subtitle_file == "/library/n/New.srt");
  CHECK(values.subtitles_visible == false);
  CHECK(values.subtitle_delay_ms == -40);
  CHECK(values.subtitle_font_family == "DejaVu Sans");
  CHECK(values.subtitle_font_size_pt == 30);
  CHECK(values.subtitle_font_color == 0xFF112233u);
}

TEST_CASE("capture and web write-back round-trips") {
  const TempDir dir;
  const auto path = dir.File("[capture]\ndevice = /dev/video0\n");

  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);

  config->SetDevice("/dev/video3");
  config->SetWebEnabled(true);
  config->SetWebRoot("/srv/www");
  config->SetApiKey("s3cret");
  REQUIRE(config->Save());

  const auto reloaded = subtitler::Config::Load(path);
  REQUIRE(reloaded != nullptr);
  const auto& values = reloaded->values();
  CHECK(values.device == "/dev/video3");
  CHECK(values.web == true);
  CHECK(values.web_root == "/srv/www");
  CHECK(values.api_key == "s3cret");
}

TEST_CASE("whisper write-back round-trips") {
  const TempDir dir;
  const auto path = dir.File("[whisper]\nenabled = false\n");

  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);

  config->SetWhisperEnabled(true);
  config->SetWhisperModel("ggml-base.en.bin");
  REQUIRE(config->Save());

  const auto reloaded = subtitler::Config::Load(path);
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->values().whisper_enabled == true);
  CHECK(reloaded->values().whisper_model == "ggml-base.en.bin");
}

TEST_CASE("clearing the whisper model removes the key") {
  const TempDir dir;
  const auto path = dir.File("[whisper]\nenabled = false\nmodel = ggml-tiny.en.bin\n");

  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);
  REQUIRE(config->values().whisper_model == "ggml-tiny.en.bin");

  config->ClearWhisperModel();
  CHECK(config->values().whisper_model == std::nullopt);
  REQUIRE(config->Save());

  const auto reloaded = subtitler::Config::Load(path);
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->values().whisper_model == std::nullopt);
}

TEST_CASE("clearing the subtitle file removes the key") {
  const TempDir dir;
  const auto path = dir.File("[subtitles]\nfile = /library/m/Movie.srt\n");

  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);

  config->SetSubtitleFile(std::nullopt);
  CHECK(config->values().subtitle_file == std::nullopt);
  REQUIRE(config->Save());

  const auto reloaded = subtitler::Config::Load(path);
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->values().subtitle_file == std::nullopt);
}

TEST_CASE("Save creates missing parent directories") {
  const TempDir dir;
  const auto path = dir.path / "nested" / "deeper" / "config.ini";
  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);

  config->SetSubtitlesVisible(true);
  REQUIRE(config->Save());

  const auto reloaded = subtitler::Config::Load(path);
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->values().subtitles_visible == true);
}

// doctest runs its cases sequentially, so moving the process working
// directory inside one case is safe as long as it is restored.
struct CwdGuard {
  explicit CwdGuard(const std::filesystem::path& path)
      : previous{std::filesystem::current_path()} {
    std::filesystem::current_path(path);
  }
  ~CwdGuard() { std::filesystem::current_path(previous); }
  std::filesystem::path previous;
};

TEST_CASE("Save works with relative config paths") {
  const TempDir dir;
  const CwdGuard cwd{dir.path};

  SUBCASE("bare filename") {
    const auto config = subtitler::Config::Load("config.ini");
    REQUIRE(config != nullptr);
    config->SetWebEnabled(true);
    REQUIRE(config->Save());

    const auto reloaded = subtitler::Config::Load(dir.path / "config.ini");
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->values().web == true);
  }

  SUBCASE("./ prefixed filename") {
    const auto config = subtitler::Config::Load("./config.ini");
    REQUIRE(config != nullptr);
    config->SetWebEnabled(true);
    REQUIRE(config->Save());
    CHECK(std::filesystem::is_regular_file(dir.path / "config.ini"));
  }

  SUBCASE("nested relative path") {
    const auto config = subtitler::Config::Load("nested/deeper/config.ini");
    REQUIRE(config != nullptr);
    config->SetWebEnabled(true);
    REQUIRE(config->Save());

    const auto reloaded =
        subtitler::Config::Load(dir.path / "nested" / "deeper" / "config.ini");
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->values().web == true);
  }
}

TEST_CASE("a failed Save is reported and preserves the committed file") {
  const TempDir dir;
  const auto path = dir.path / "config.ini";

  const auto config = subtitler::Config::Load(path);
  REQUIRE(config != nullptr);
  config->SetWebEnabled(true);
  REQUIRE(config->Save());
  const std::string committed = ReadFile(path);

  SUBCASE("the staged write fails") {
    // A zero file-size limit fails every write, the final flush
    // included, root or not; SIGXFSZ must not kill the test.
    struct rlimit previous {};
    REQUIRE(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    // Soft limit only: lowering the hard one would be irreversible.
    const struct rlimit zero { 0, previous.rlim_max };
    REQUIRE(setrlimit(RLIMIT_FSIZE, &zero) == 0);
    auto* previous_handler = std::signal(SIGXFSZ, SIG_IGN);

    config->SetWebEnabled(false);
    CHECK_FALSE(config->Save());

    std::signal(SIGXFSZ, previous_handler);
    REQUIRE(setrlimit(RLIMIT_FSIZE, &previous) == 0);

    CHECK(ReadFile(path) == committed);
    CHECK(!std::filesystem::exists(dir.path / "config.ini.tmp"));
  }

  SUBCASE("the destination can't be replaced") {
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);

    config->SetWebEnabled(false);
    CHECK_FALSE(config->Save());

    CHECK(std::filesystem::is_directory(path));
    CHECK(!std::filesystem::exists(dir.path / "config.ini.tmp"));
  }
}
