#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

typedef struct _GKeyFile GKeyFile;

namespace subtitler {

// The appliance configuration (#16): an INI file, conventionally at
// <ConfigDirectory()>/config.ini, overridable with --config=<path>.
// Every command-line option has a key (the command line wins); the
// [subtitles] group additionally persists the style state changed
// through the web API, and an explicitly supplied video-device, --web,
// --web-root, or --api-key is persisted too, all written back
// atomically (#221). See config/subtitler.example.ini for the
// documented key set.
class Config {
 public:
  // The parsed key set; nullopt for keys the file doesn't set or sets
  // to an invalid value (those are dropped with a log warning, #220).
  struct Values {
    // [capture]
    std::optional<std::string> device;
    std::optional<bool> audio;
    // [output]
    std::optional<std::string> output_mode;  // software|pisp|window|null
    std::optional<int> connector_id;
    std::optional<std::string> audio_output_device;
    std::optional<int> audio_offset_ms;
    // [web]
    std::optional<bool> web;
    std::optional<std::string> web_root;
    std::optional<std::string> api_key;
    // [subtitles]
    std::optional<std::string> subtitle_file;
    std::optional<bool> subtitles_visible;
    std::optional<std::int64_t> subtitle_delay_ms;
    std::optional<std::string> subtitle_font_family;
    std::optional<std::int64_t> subtitle_font_size_pt;
    // font-color is written as #rrggbb; the value is big-endian ARGB.
    std::optional<std::uint32_t> subtitle_font_color;
    // [whisper]
    std::optional<bool> whisper_enabled;
    // A model file name inside <state-dir>/models (#19).
    std::optional<std::string> whisper_model;
  };

  // Loads path. A missing file yields an empty document (Save creates
  // it on first write-back); a malformed file or an unsupported
  // [subtitler] version yields nullopt — the appliance runs on defaults
  // and never clobbers a file it couldn't parse.
  static std::unique_ptr<Config> Load(const std::filesystem::path& path);

  const Values& values() const { return values_; }

  // Write-back for the web-API subtitle state (#222/#223/#224): updates
  // both the in-memory document and values(). Unknown keys and comments
  // survive the round-trip.
  void SetSubtitleFile(const std::optional<std::string>& file);
  void SetSubtitlesVisible(bool visible);
  void SetSubtitleDelayMs(std::int64_t delay_ms);
  void SetSubtitleFontFamily(std::string_view family);
  void SetSubtitleFontSizePt(std::int64_t size_pt);
  void SetSubtitleFontColor(std::uint32_t color_argb);

  // Write-back for the web-API whisper state (#19).
  void SetWhisperEnabled(bool enabled);
  void SetWhisperModel(std::string_view model);

  // Write-back for values explicitly supplied on the command line, so
  // the next run needs no flags.
  void SetDevice(std::string_view device);
  void SetWebEnabled(bool enabled);
  void SetWebRoot(std::string_view root);
  void SetApiKey(std::string_view api_key);

  // Serializes the document to the loaded path atomically (temp file +
  // rename), creating parent directories. false on any I/O failure.
  bool Save() const;

 private:
  struct KeyFileDeleter {
    void operator()(GKeyFile* key_file) const noexcept;
  };

  Config(const std::filesystem::path& path, GKeyFile* key_file, Values values)
      : path_(path), key_file_(key_file), values_(std::move(values)) {}

  std::filesystem::path path_;
  std::unique_ptr<GKeyFile, KeyFileDeleter> key_file_;
  Values values_;
};

}  // namespace subtitler
