#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "web/subtitle_routes.h"

namespace subtitler {

class PreviewFrameBuffer;

// Everything the server needs from the appliance, injected by main.cpp;
// an unset hook disables its endpoints. Hooks are called on the server's
// io thread, so they may block clients briefly.
struct WebServerHooks {
  // Called when the MJPEG client count transitions between zero and
  // nonzero, so no JPEG encoding happens without watchers.
  std::function<void(bool)> preview_activation;
  // PUT /api/subtitles/<title> (#212).
  SubtitleUploadHandler subtitle_upload;
  // The static file fallback (#212); without it unmatched paths are 404.
  std::optional<std::filesystem::path> web_root;
};

// The appliance web server (docs/rest-api.md): the MJPEG preview
// endpoints, the subtitle upload endpoint, and a static file fallback
// for the #15 web interface. Routes are organized per feature in
// preview_routes/subtitle_routes/static_files; this class is lifecycle
// only.
class WebServer {
  struct Implementation;

 public:
  // Binds all interfaces on port. Returns nullptr when the port cannot
  // be bound. frames must outlive the server.
  static std::unique_ptr<WebServer> Create(std::uint16_t port,
                                           PreviewFrameBuffer& frames,
                                           WebServerHooks hooks = {});
  ~WebServer();

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
