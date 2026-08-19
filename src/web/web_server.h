#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace subtitler {

class PreviewFrameBuffer;

// The appliance web server. Serves the MJPEG preview endpoints from
// docs/video-output.md: GET /api/preview.jpg (newest frame),
// GET /api/preview.mjpeg (multipart stream, newest-frame-only per client),
// and GET / (minimal page showing the stream). Not the #15 web interface.
class WebServer {
  struct Implementation;

 public:
  // Binds all interfaces on port. Returns nullptr when the port cannot be
  // bound. frames must outlive the server. preview_activation is called
  // (on the server's io thread) when the MJPEG client count transitions
  // between zero and nonzero, so no JPEG encoding happens without
  // watchers.
  static std::unique_ptr<WebServer> Create(
      std::uint16_t port, PreviewFrameBuffer& frames,
      std::function<void(bool)> preview_activation = {});
  ~WebServer();

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
