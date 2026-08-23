#include "config/config.h"

#include <glib.h>

#include <charconv>
#include <format>
#include <fstream>
#include <print>
#include <system_error>
#include <utility>

#include "utils/logging.h"
#include "utils/unique_ptr.h"

namespace subtitler {

namespace {

constexpr std::int64_t kSupportedVersion = 1;

void WarnDroppedKey(const char* group, const char* key,
                    const std::string& value) {
  CONFIG_LOG(LogLevel::kWarning, "Ignoring invalid config value {}.{} = {}",
             group, key, value);
}

// nullopt for a missing or empty key; any string is a valid value.
std::optional<std::string> GetString(GKeyFile* key_file, const char* group,
                                     const char* key) {
  gchar* raw = g_key_file_get_string(key_file, group, key, nullptr);
  if (raw == nullptr) {
    return std::nullopt;
  }

  std::string value{raw};
  g_free(raw);
  if (value.empty()) {
    return std::nullopt;
  }

  return value;
}

std::optional<bool> GetBoolean(GKeyFile* key_file, const char* group,
                               const char* key) {
  if (g_key_file_has_key(key_file, group, key, nullptr) == 0) {
    return std::nullopt;
  }

  GError* error = nullptr;
  const gboolean value = g_key_file_get_boolean(key_file, group, key, &error);
  if (error != nullptr) {
    WarnDroppedKey(group, key, error->message);
    g_error_free(error);
    return std::nullopt;
  }

  return value != 0;
}

std::optional<int> GetInteger(GKeyFile* key_file, const char* group,
                              const char* key) {
  if (g_key_file_has_key(key_file, group, key, nullptr) == 0) {
    return std::nullopt;
  }

  GError* error = nullptr;
  const int value = g_key_file_get_integer(key_file, group, key, &error);
  if (error != nullptr) {
    WarnDroppedKey(group, key, error->message);
    g_error_free(error);
    return std::nullopt;
  }

  return value;
}

std::optional<std::int64_t> GetInteger64(GKeyFile* key_file, const char* group,
                                         const char* key) {
  if (g_key_file_has_key(key_file, group, key, nullptr) == 0) {
    return std::nullopt;
  }

  GError* error = nullptr;
  const gint64 value = g_key_file_get_int64(key_file, group, key, &error);
  if (error != nullptr) {
    WarnDroppedKey(group, key, error->message);
    g_error_free(error);
    return std::nullopt;
  }

  return value;
}

// "#rrggbb", mirroring the web API's font_color parameter; the alpha is
// always opaque.
std::optional<std::uint32_t> GetColor(GKeyFile* key_file, const char* group,
                                      const char* key) {
  const auto text = GetString(key_file, group, key);
  if (!text) {
    return std::nullopt;
  }

  std::uint32_t rgb = 0;
  if (text->size() == 7 && text->front() == '#') {
    const auto [end, error] =
        std::from_chars(text->data() + 1, text->data() + text->size(), rgb, 16);
    if (error == std::errc{} && end == text->data() + text->size()) {
      return 0xFF'00'00'00 | rgb;
    }
  }

  WarnDroppedKey(group, key, *text);
  return std::nullopt;
}

}  // namespace

void Config::KeyFileDeleter::operator()(GKeyFile* key_file) const noexcept {
  if (key_file != nullptr) {
    g_key_file_unref(key_file);
  }
}

std::unique_ptr<Config> Config::Load(const std::filesystem::path& path) {
  UniquePtr<GKeyFile, g_key_file_unref> key_file{g_key_file_new()};

  GError* error = nullptr;
  if (g_key_file_load_from_file(key_file.get(), path.c_str(),
                                G_KEY_FILE_KEEP_COMMENTS, &error) == 0) {
    // A missing file is a fresh, writable document; anything else is a
    // file we must not clobber.
    if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT) == 0) {
      std::println(stderr, "Ignoring unusable config file {}: {}",
                   path.string(), error->message);
      g_error_free(error);
      return nullptr;
    }
    g_error_free(error);
  }

  if (g_key_file_has_key(key_file.get(), "subtitler", "version", nullptr) !=
      0) {
    const auto version = GetInteger64(key_file.get(), "subtitler", "version");
    if (version != kSupportedVersion) {
      std::println(stderr,
                   "Ignoring config file {}: unsupported version (expected "
                   "{})",
                   path.string(), kSupportedVersion);
      return nullptr;
    }
  }

  Values values;
  values.device = GetString(key_file.get(), "capture", "device");
  values.audio = GetBoolean(key_file.get(), "capture", "audio");
  values.output_mode = GetString(key_file.get(), "output", "mode");
  values.connector_id = GetInteger(key_file.get(), "output", "connector-id");
  values.audio_output_device =
      GetString(key_file.get(), "output", "audio-device");
  values.audio_offset_ms =
      GetInteger(key_file.get(), "output", "audio-offset-ms");
  values.web = GetBoolean(key_file.get(), "web", "enabled");
  values.web_root = GetString(key_file.get(), "web", "root");
  values.api_key = GetString(key_file.get(), "web", "api-key");
  values.subtitle_file = GetString(key_file.get(), "subtitles", "file");
  values.subtitles_visible = GetBoolean(key_file.get(), "subtitles", "visible");
  values.subtitle_delay_ms =
      GetInteger64(key_file.get(), "subtitles", "delay-ms");
  values.subtitle_font_family =
      GetString(key_file.get(), "subtitles", "font-family");
  values.subtitle_font_size_pt =
      GetInteger64(key_file.get(), "subtitles", "font-size-pt");
  values.subtitle_font_color =
      GetColor(key_file.get(), "subtitles", "font-color");

  return std::unique_ptr<Config>{
      new Config{path, key_file.release(), std::move(values)}};
}

void Config::SetSubtitleFile(const std::optional<std::string>& file) {
  if (file) {
    g_key_file_set_string(key_file_.get(), "subtitles", "file", file->c_str());
  } else {
    g_key_file_remove_key(key_file_.get(), "subtitles", "file", nullptr);
  }
  values_.subtitle_file = file;
}

void Config::SetSubtitlesVisible(bool visible) {
  g_key_file_set_boolean(key_file_.get(), "subtitles", "visible",
                         static_cast<gboolean>(visible));
  values_.subtitles_visible = visible;
}

void Config::SetSubtitleDelayMs(std::int64_t delay_ms) {
  g_key_file_set_int64(key_file_.get(), "subtitles", "delay-ms",
                       static_cast<gint64>(delay_ms));
  values_.subtitle_delay_ms = delay_ms;
}

void Config::SetSubtitleFontFamily(std::string_view family) {
  g_key_file_set_string(key_file_.get(), "subtitles", "font-family",
                        std::string{family}.c_str());
  values_.subtitle_font_family = std::string{family};
}

void Config::SetSubtitleFontSizePt(std::int64_t size_pt) {
  g_key_file_set_int64(key_file_.get(), "subtitles", "font-size-pt",
                       static_cast<gint64>(size_pt));
  values_.subtitle_font_size_pt = size_pt;
}

void Config::SetSubtitleFontColor(std::uint32_t color_argb) {
  g_key_file_set_string(
      key_file_.get(), "subtitles", "font-color",
      std::format("#{:06x}", color_argb & 0xFF'FF'FF).c_str());
  values_.subtitle_font_color = color_argb;
}

bool Config::Save() const {
  gsize length = 0;
  const UniquePtr<gchar, g_free> data{
      g_key_file_to_data(key_file_.get(), &length, nullptr)};
  if (data == nullptr) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) {
    return false;
  }

  std::filesystem::path temp = path_;
  temp += ".tmp";
  {
    std::ofstream file{temp, std::ios::binary | std::ios::trunc};
    file.write(data.get(), static_cast<std::streamsize>(length));
    if (!file) {
      return false;
    }
  }

  std::filesystem::rename(temp, path_, error);
  return !error;
}

}  // namespace subtitler
