#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace subtitler {

class PreviewFrameBuffer;

// The result of handing an uploaded subtitle to the appliance (#212).
enum class SubtitleUploadStatus {
  kStored,        // saved to the library and activated on the stream
  kInvalidTitle,  // not a usable library name
  kFailed,        // storage or activation failed
};

struct SubtitleUploadResult {
  SubtitleUploadStatus status;
  // The library-relative name (e.g. "m/Movie.srt"); set when kStored.
  std::string stored_name;
};

// Handles an uploaded SRT on the server's io thread: title is the bare
// library filename, contents the raw SRT text.
using SubtitleUploadHandler = std::function<SubtitleUploadResult(
    std::string_view title, std::string_view contents)>;

// The appliance web server. Serves the MJPEG preview endpoints from
// docs/video-output.md: GET /api/preview.jpg (newest frame),
// GET /api/preview.mjpeg (multipart stream, newest-frame-only per
// client), and PUT /api/subtitles/<title> (SRT upload, #212). Requests
// without a registered route fall back to static .html/.js/.css/.png
// files under the web root ("/" maps to index.html); the #15 web
// interface is static files.
class WebServer {
  struct Implementation;

 public:
  // Binds all interfaces on port. Returns nullptr when the port cannot
  // be bound. frames must outlive the server. preview_activation is
  // called (on the server's io thread) when the MJPEG client count
  // transitions between zero and nonzero, so no JPEG encoding happens
  // without watchers. subtitle_upload handles PUT /api/subtitles/<title>
  // (on the io thread, so it may block clients briefly); without it the
  // endpoint is not registered. web_root enables the static file
  // fallback; without it unmatched paths are 404.
  static std::unique_ptr<WebServer> Create(
      std::uint16_t port, PreviewFrameBuffer& frames,
      std::function<void(bool)> preview_activation = {},
      SubtitleUploadHandler subtitle_upload = {},
      std::optional<std::filesystem::path> web_root = std::nullopt);
  ~WebServer();

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
