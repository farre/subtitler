#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _GBytes GBytes;
typedef struct _GMainContext GMainContext;
typedef struct _SoupServer SoupServer;
typedef struct _SoupServerMessage SoupServerMessage;

namespace subtitler {

class PreviewFrameBuffer;

// The MJPEG preview endpoints from docs/video-output.md:
// GET /api/preview.jpg (newest frame) and GET /api/preview.mjpeg
// (multipart stream, newest-frame-only per client). A watcher thread
// waits on the shared frame buffer and posts deliveries onto the server
// context; SoupServerMessage objects are only ever touched on that
// context. The client count toggles the preview gate at the
// zero/nonzero transitions, so no JPEG encoding happens without
// watchers. Internal to the web module; web_server.cpp composes the
// route modules.
struct PreviewRoutes {
  // One MJPEG client. All fields are touched only on the io thread.
  struct Client {
    PreviewRoutes* owner;
    SoupServerMessage* message;  // borrowed; valid until "finished"

    // At most one frame is in flight and one newer frame is pending; a
    // slow client skips frames rather than building a backlog.
    GBytes* pending = nullptr;
    std::uint64_t pending_sequence = 0;
    bool in_flight = false;
    std::uint64_t last_sent = 0;

    ~Client();
  };

  // frames must outlive this. activation is called on the io thread.
  PreviewRoutes(PreviewFrameBuffer& frames,
                std::function<void(bool)> activation);
  ~PreviewRoutes();

  // Adds the route handlers with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);
  // Spawns the watcher thread posting deliveries to context.
  void Start(GMainContext* context);
  // Joins the watcher; after Stop no new deliveries are posted.
  void Stop();

  PreviewFrameBuffer& frames_;
  std::function<void(bool)> activation_;
  GMainContext* context_ = nullptr;  // borrowed from the server
  std::vector<std::unique_ptr<Client>> clients_;  // io thread only
  std::jthread watcher_thread_;
};

}  // namespace subtitler
