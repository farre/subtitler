#include "web/web_server.h"

#include <libsoup/soup.h>

#include <cstdint>
#include <future>
#include <memory>
#include <print>
#include <thread>
#include <utility>

#include "web/opensubtitles_routes.h"
#include "web/preview_routes.h"
#include "web/static_files.h"

struct subtitler::WebServer::Implementation {
  Implementation(std::uint16_t port, PreviewFrameBuffer& frames,
                 WebServerHooks hooks)
      : preview_{frames, std::move(hooks.preview_activation)},
        static_files_{std::move(hooks.web_root)} {
    subtitle_routes_.upload_ = std::move(hooks.subtitle_upload);
    subtitle_routes_.get_ = std::move(hooks.subtitle_get);
    subtitle_routes_.list_ = std::move(hooks.subtitle_list);
    subtitle_routes_.state_get_ = std::move(hooks.subtitle_state_get);
    subtitle_routes_.state_set_ = std::move(hooks.subtitle_state_set);
    subtitle_routes_.sync_get_ = std::move(hooks.subtitle_sync_get);
    subtitle_routes_.sync_start_ = std::move(hooks.subtitle_sync_start);
    font_routes_.list_ = std::move(hooks.font_list);
    whisper_routes_.state_get_ = std::move(hooks.whisper_state_get);
    whisper_routes_.state_set_ = std::move(hooks.whisper_state_set);
    whisper_routes_.state_dir_ = std::move(hooks.state_dir);
    opensubtitles_routes_.api_key_ = std::move(hooks.api_key);

    std::promise<bool> ready;

    io_thread_ = std::jthread{[this, port, &ready] { Run(port, ready); }};

    started_ = ready.get_future().get();

    if (!started_) {
      io_thread_.join();
      return;
    }

    preview_.Start(context_);
  }

  ~Implementation() {
    if (!started_) {
      return;
    }

    // Join the watcher first: after this no new deliveries are posted,
    // and the io thread can drain the ones still queued.
    preview_.Stop();

    g_main_loop_quit(loop_);
    io_thread_.join();
  }

  void Run(std::uint16_t port, std::promise<bool>& ready) {
    context_ = g_main_context_new();
    g_main_context_push_thread_default(context_);
    loop_ = g_main_loop_new(context_, FALSE);

    server_ = soup_server_new("server-header", "subtitler", nullptr);

    preview_.Register(server_);
    subtitle_routes_.Register(server_);
    font_routes_.Register(server_);
    whisper_routes_.Register(server_);
    opensubtitles_routes_.Register(server_);
    static_files_.Register(server_);

    GError* error = nullptr;
    const bool listening = soup_server_listen_all(
        server_, port, static_cast<SoupServerListenOptions>(0), &error);

    if (!listening) {
      std::println(stderr, "Web server could not listen on port {}: {}", port,
                   error != nullptr ? error->message : "unknown error");
      g_clear_error(&error);
    }

    ready.set_value(listening);

    if (listening) {
      g_main_loop_run(loop_);

      // Deliveries posted before the watcher joined may still be queued;
      // run them out so their destroy notifies free the GBytes.
      while (g_main_context_pending(context_)) {
        g_main_context_iteration(context_, FALSE);
      }
    }

    soup_server_disconnect(server_);
    g_object_unref(server_);
    g_main_loop_unref(loop_);
    g_main_context_pop_thread_default(context_);
    g_main_context_unref(context_);
  }

  PreviewRoutes preview_;
  SubtitleRoutes subtitle_routes_;
  FontRoutes font_routes_;
  WhisperRoutes whisper_routes_;
  OpenSubtitlesRoutes opensubtitles_routes_;
  StaticFiles static_files_;

  // These three are created, used, and destroyed on the io thread; the
  // destructor only calls g_main_loop_quit on loop_ from the outside.
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  SoupServer* server_ = nullptr;

  std::jthread io_thread_;
  bool started_ = false;
};

namespace subtitler {

/* static */
std::unique_ptr<WebServer> WebServer::Create(std::uint16_t port,
                                             PreviewFrameBuffer& frames,
                                             WebServerHooks hooks) {
  auto server = std::make_unique<WebServer>();
  server->implementation_ =
      std::make_unique<Implementation>(port, frames, std::move(hooks));

  if (!server->implementation_->started_) {
    return nullptr;
  }

  return server;
}

WebServer::~WebServer() = default;

}  // namespace subtitler
