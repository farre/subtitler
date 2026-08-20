#include "web/web_server.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "utils/logging.h"
#include "utils/preview_frame.h"
#include "utils/unique_ptr.h"

namespace {

using namespace subtitler;

// Per docs/video-output.md.
constexpr std::string_view kBoundary = "frame";
constexpr std::size_t kMaxClients = 4;

// Uploads (#212).
constexpr std::string_view kSubtitlesPrefix = "/api/subtitles/";
constexpr std::size_t kMaxSubtitleBytes = 8 * 1024 * 1024;

GBytes* MakePart(const PreviewFrame& frame) {
  const std::string header = std::format(
      "--{}\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: {}\r\n"
      "X-Frame-Sequence: {}\r\n"
      "\r\n",
      kBoundary, frame.data->size(), frame.sequence);

  const std::size_t total = header.size() + frame.data->size() + 2;
  auto* bytes = static_cast<std::byte*>(g_malloc(total));

  std::memcpy(bytes, header.data(), header.size());
  std::memcpy(bytes + header.size(), frame.data->data(), frame.data->size());
  bytes[total - 2] = std::byte{'\r'};
  bytes[total - 1] = std::byte{'\n'};

  return g_bytes_new_take(bytes, total);
}

// Only .html, .js, .css, and .png are ever served (#212): the allowlist
// doubles as the MIME map.
const char* StaticContentType(std::string_view path) {
  if (path.ends_with(".html")) {
    return "text/html; charset=utf-8";
  }
  if (path.ends_with(".js")) {
    return "text/javascript; charset=utf-8";
  }
  if (path.ends_with(".css")) {
    return "text/css; charset=utf-8";
  }
  if (path.ends_with(".png")) {
    return "image/png";
  }
  return nullptr;
}

}  // namespace

struct subtitler::WebServer::Implementation {
  // One MJPEG client. All fields are touched only on the io thread.
  struct Client {
    Implementation* owner;
    SoupServerMessage* message;  // borrowed; valid until "finished"

    // At most one frame is in flight and one newer frame is pending; a
    // slow client skips frames rather than building a backlog.
    GBytes* pending = nullptr;
    std::uint64_t pending_sequence = 0;
    bool in_flight = false;
    std::uint64_t last_sent = 0;

    ~Client() {
      if (pending != nullptr) {
        g_bytes_unref(pending);
      }
    }
  };

  struct Delivery {
    Implementation* self;
    GBytes* part;
    std::uint64_t sequence;
  };

  Implementation(std::uint16_t port, PreviewFrameBuffer& frames,
                 std::function<void(bool)> preview_activation,
                 SubtitleUploadHandler subtitle_upload,
                 std::optional<std::filesystem::path> web_root)
      : frames_{frames},
        preview_activation_{std::move(preview_activation)},
        subtitle_upload_{std::move(subtitle_upload)},
        web_root_{std::move(web_root)} {
    std::promise<bool> ready;

    io_thread_ = std::jthread{[this, port, &ready] { Run(port, ready); }};

    started_ = ready.get_future().get();

    if (!started_) {
      io_thread_.join();
      return;
    }

    watcher_thread_ =
        std::jthread{[this](std::stop_token stop) { WatchFrames(stop); }};
  }

  ~Implementation() {
    if (!started_) {
      return;
    }

    // Join the watcher first: after this no new deliveries are posted, and
    // the io thread can drain the ones still queued.
    watcher_thread_.request_stop();
    watcher_thread_.join();

    g_main_loop_quit(loop_);
    io_thread_.join();
  }

  void Run(std::uint16_t port, std::promise<bool>& ready) {
    context_ = g_main_context_new();
    g_main_context_push_thread_default(context_);
    loop_ = g_main_loop_new(context_, FALSE);

    server_ = soup_server_new("server-header", "subtitler", nullptr);
    soup_server_add_handler(server_, "/api/preview.jpg", HandleJpg, this,
                            nullptr);
    soup_server_add_handler(server_, "/api/preview.mjpeg", HandleMjpeg, this,
                            nullptr);

    if (subtitle_upload_) {
      soup_server_add_handler(server_, "/api/subtitles", HandleSubtitleUpload,
                              this, nullptr);
    }

    // The default handler: every unmatched path tries the web root.
    if (web_root_) {
      soup_server_add_handler(server_, "/", HandleStatic, this, nullptr);
    }

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

  // Producer side: wakes on every newly stored frame and hands its
  // multipart part to the io thread. Never touches SoupServerMessage
  // objects off the server context.
  void WatchFrames(std::stop_token stop) {
    std::uint64_t last_seen = 0;

    while (!stop.stop_requested()) {
      const auto frame = frames_.WaitNewer(last_seen, stop);

      if (!frame) {
        break;
      }

      last_seen = frame->sequence;

      auto* delivery = new Delivery{this, MakePart(*frame), frame->sequence};
      g_main_context_invoke_full(context_, G_PRIORITY_DEFAULT, Deliver,
                                 delivery, DestroyDelivery);
    }
  }

  static gboolean Deliver(gpointer user_data) {
    auto* delivery = static_cast<Delivery*>(user_data);

    for (const auto& holder : delivery->self->clients_) {
      Client& client = *holder;

      if (delivery->sequence <= client.last_sent) {
        continue;
      }

      if (client.in_flight) {
        // This client's socket is behind; keep only the newest frame.
        if (client.pending != nullptr) {
          g_bytes_unref(client.pending);
        }

        client.pending = g_bytes_ref(delivery->part);
        client.pending_sequence = delivery->sequence;
      } else {
        SendPart(client, delivery->part, delivery->sequence);
      }
    }

    return G_SOURCE_REMOVE;
  }

  static void DestroyDelivery(gpointer user_data) {
    auto* delivery = static_cast<Delivery*>(user_data);
    g_bytes_unref(delivery->part);
    delete delivery;
  }

  // soup_message_body_append_bytes takes its own reference.
  static void SendPart(Client& client, GBytes* part, std::uint64_t sequence) {
    client.in_flight = true;
    client.last_sent = sequence;

    soup_message_body_append_bytes(
        soup_server_message_get_response_body(client.message), part);
    soup_server_message_unpause(client.message);
  }

  static void OnWroteChunk(SoupServerMessage*, gpointer user_data) {
    auto& client = *static_cast<Client*>(user_data);
    client.in_flight = false;

    if (client.pending == nullptr) {
      return;
    }

    GBytes* next = std::exchange(client.pending, nullptr);
    SendPart(client, next, client.pending_sequence);
    g_bytes_unref(next);
  }

  static void OnFinished(SoupServerMessage*, gpointer user_data) {
    auto* client = static_cast<Client*>(user_data);
    auto& self = *client->owner;

    std::erase_if(self.clients_, [client](const auto& holder) {
      return holder.get() == client;
    });

    // The last client went away; stop the JPEG encoder.
    if (self.clients_.empty()) {
      WEB_LOG(LogLevel::kInfo, "Stopped serving MJPEG");
      if (self.preview_activation_) {
        self.preview_activation_(false);
      }
    }
  }

  static void HandleJpg(SoupServer*, SoupServerMessage* message, const char*,
                        GHashTable*, gpointer user_data) {
    auto& self = *static_cast<Implementation*>(user_data);

    const auto frame = self.frames_.Latest();

    // Only reachable if placeholder seeding failed at startup.
    if (!frame) {
      soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE,
                                     nullptr);
      return;
    }

    soup_server_message_set_response(
        message, "image/jpeg", SOUP_MEMORY_COPY,
        reinterpret_cast<const char*>(frame->data->data()),
        frame->data->size());

    auto* headers = soup_server_message_get_response_headers(message);
    soup_message_headers_replace(headers, "Cache-Control", "no-store");
    const auto sequence = std::to_string(frame->sequence);
    soup_message_headers_replace(headers, "X-Frame-Sequence", sequence.c_str());

    soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
  }

  static void HandleMjpeg(SoupServer*, SoupServerMessage* message, const char*,
                          GHashTable*, gpointer user_data) {
    auto& self = *static_cast<Implementation*>(user_data);

    if (self.clients_.size() >= kMaxClients) {
      soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE,
                                     nullptr);
      return;
    }

    auto client = std::make_unique<Client>();
    client->owner = &self;
    client->message = message;

    auto* raw_client = client.get();
    self.clients_.push_back(std::move(client));

    // The first client starts the JPEG encoder.
    if (self.clients_.size() == 1) {
      WEB_LOG(LogLevel::kInfo, "Started serving MJPEG");
      if (self.preview_activation_) {
        self.preview_activation_(true);
      }
    }

    g_signal_connect(message, "wrote-chunk", G_CALLBACK(OnWroteChunk),
                     raw_client);
    g_signal_connect(message, "finished", G_CALLBACK(OnFinished), raw_client);

    auto* headers = soup_server_message_get_response_headers(message);
    const std::string content_type =
        std::format("multipart/x-mixed-replace; boundary={}", kBoundary);
    soup_message_headers_replace(headers, "Content-Type", content_type.c_str());
    soup_message_headers_replace(headers, "Cache-Control", "no-store");
    soup_message_headers_replace(headers, "Pragma", "no-cache");

    // HTTP transfer framing, separate from the MIME part framing. Written
    // chunks are discarded, so an indefinite response cannot grow.
    soup_message_headers_set_encoding(headers, SOUP_ENCODING_CHUNKED);
    soup_message_body_set_accumulate(
        soup_server_message_get_response_body(message), FALSE);

    soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);

    // No explicit pause: libsoup pauses a chunked response while no chunk
    // is available. Send the current frame right away so a client that
    // connects between stores (e.g. while only the placeholder exists) has
    // something to show.
    if (const auto frame = self.frames_.Latest()) {
      SendPart(*raw_client, MakePart(*frame), frame->sequence);
    }
  }

  // PUT /api/subtitles/<title> (#212): stores and activates the SRT in
  // the body. libsoup invokes the handler after the request body is
  // complete, so the body is fully available here.
  static void HandleSubtitleUpload(SoupServer*, SoupServerMessage* message,
                                   const char* path, GHashTable*,
                                   gpointer user_data) {
    auto& self = *static_cast<Implementation*>(user_data);

    if (std::string_view{soup_server_message_get_method(message)} != "PUT") {
      soup_message_headers_replace(
          soup_server_message_get_response_headers(message), "Allow", "PUT");
      soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                     nullptr);
      return;
    }

    // The handler is prefix-matched; the title is whatever follows the
    // prefix, percent-encoded.
    const std::string_view raw{path};
    if (!raw.starts_with(kSubtitlesPrefix) ||
        raw.size() == kSubtitlesPrefix.size()) {
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
      return;
    }

    const UniquePtr<gchar, g_free> decoded{
        g_uri_unescape_string(path + kSubtitlesPrefix.size(), nullptr)};
    if (decoded == nullptr) {
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
      return;
    }

    const UniquePtr<GBytes, g_bytes_unref> body{soup_message_body_flatten(
        soup_server_message_get_request_body(message))};

    const gsize body_size = body != nullptr ? g_bytes_get_size(body.get()) : 0;
    if (body_size == 0) {
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
      return;
    }

    if (body_size > kMaxSubtitleBytes) {
      soup_server_message_set_status(
          message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, nullptr);
      return;
    }

    gsize data_size;
    const auto* data =
        static_cast<const char*>(g_bytes_get_data(body.get(), &data_size));

    const auto result =
        self.subtitle_upload_(decoded.get(), std::string_view{data, data_size});

    switch (result.status) {
      case SubtitleUploadStatus::kStored:
        soup_server_message_set_response(
            message, "text/plain; charset=utf-8", SOUP_MEMORY_COPY,
            result.stored_name.data(), result.stored_name.size());
        soup_server_message_set_status(message, SOUP_STATUS_CREATED, nullptr);
        break;
      case SubtitleUploadStatus::kInvalidTitle:
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST,
                                       nullptr);
        break;
      case SubtitleUploadStatus::kFailed:
        soup_server_message_set_status(
            message, SOUP_STATUS_INTERNAL_SERVER_ERROR, nullptr);
        break;
    }
  }

  // The static fallback (#212): GET paths not claimed by a registered
  // route are served from the web root; "/" maps to index.html.
  static void HandleStatic(SoupServer*, SoupServerMessage* message,
                           const char* path, GHashTable*, gpointer user_data) {
    auto& self = *static_cast<Implementation*>(user_data);

    const auto not_found = [&] {
      soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    };

    if (std::string_view{soup_server_message_get_method(message)} != "GET") {
      not_found();
      return;
    }

    const UniquePtr<gchar, g_free> decoded{
        g_uri_unescape_string(path, nullptr)};
    if (decoded == nullptr) {
      not_found();
      return;
    }

    std::string_view relative{decoded.get()};
    if (relative == "/") {
      relative = "/index.html";
    }

    const char* content_type = StaticContentType(relative);
    if (!relative.starts_with('/') || content_type == nullptr) {
      not_found();
      return;
    }

    // Traversal guard: only plain segments, joined under the web root.
    std::filesystem::path file = *self.web_root_;
    bool valid = true;
    for (const auto segment : std::views::split(relative.substr(1), '/')) {
      const std::string_view part{segment};
      if (part.empty() || part == "." || part == ".." ||
          part.find('\\') != std::string_view::npos) {
        valid = false;
        break;
      }
      file /= part;
    }

    std::error_code error;
    if (!valid || !std::filesystem::is_regular_file(file, error)) {
      not_found();
      return;
    }

    std::ifstream stream{file, std::ios::binary};
    const std::string content{std::istreambuf_iterator<char>{stream},
                              std::istreambuf_iterator<char>{}};
    if (stream.bad()) {
      not_found();
      return;
    }

    soup_server_message_set_response(message, content_type, SOUP_MEMORY_COPY,
                                     content.data(), content.size());
    soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
  }

  PreviewFrameBuffer& frames_;
  std::function<void(bool)> preview_activation_;
  SubtitleUploadHandler subtitle_upload_;
  std::optional<std::filesystem::path> web_root_;

  // These three are created, used, and destroyed on the io thread; the
  // destructor only calls g_main_loop_quit on loop_ from the outside.
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  SoupServer* server_ = nullptr;

  std::vector<std::unique_ptr<Client>> clients_;  // io thread only

  std::jthread io_thread_;
  std::jthread watcher_thread_;
  bool started_ = false;
};

namespace subtitler {

/* static */
std::unique_ptr<WebServer> WebServer::Create(
    std::uint16_t port, PreviewFrameBuffer& frames,
    std::function<void(bool)> preview_activation,
    SubtitleUploadHandler subtitle_upload,
    std::optional<std::filesystem::path> web_root) {
  auto server = std::make_unique<WebServer>();
  server->implementation_ = std::make_unique<Implementation>(
      port, frames, std::move(preview_activation), std::move(subtitle_upload),
      std::move(web_root));

  if (!server->implementation_->started_) {
    return nullptr;
  }

  return server;
}

WebServer::~WebServer() = default;

}  // namespace subtitler
