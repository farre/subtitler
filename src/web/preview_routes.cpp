#include "web/preview_routes.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "utils/logging.h"
#include "utils/preview_frame.h"

namespace {

using namespace subtitler;

// Per docs/video-output.md.
constexpr std::string_view kBoundary = "frame";
constexpr std::size_t kMaxClients = 4;

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

struct Delivery {
  PreviewRoutes* self;
  GBytes* part;
  std::uint64_t sequence;
};

// soup_message_body_append_bytes takes its own reference.
void SendPart(PreviewRoutes::Client& client, GBytes* part,
              std::uint64_t sequence) {
  client.in_flight = true;
  client.last_sent = sequence;

  soup_message_body_append_bytes(
      soup_server_message_get_response_body(client.message), part);
  soup_server_message_unpause(client.message);
}

void OnWroteChunk(SoupServerMessage*, gpointer user_data) {
  auto& client = *static_cast<PreviewRoutes::Client*>(user_data);
  client.in_flight = false;

  if (client.pending == nullptr) {
    return;
  }

  GBytes* next = std::exchange(client.pending, nullptr);
  SendPart(client, next, client.pending_sequence);
  g_bytes_unref(next);
}

void OnFinished(SoupServerMessage*, gpointer user_data) {
  auto* client = static_cast<PreviewRoutes::Client*>(user_data);
  auto& self = *client->owner;

  std::erase_if(self.clients_,
                [client](const auto& holder) { return holder.get() == client; });

  // The last client went away; stop the JPEG encoder.
  if (self.clients_.empty()) {
    WEB_LOG(LogLevel::kInfo, "Stopped serving MJPEG");
    if (self.activation_) {
      self.activation_(false);
    }
  }
}

gboolean Deliver(gpointer user_data) {
  auto* delivery = static_cast<Delivery*>(user_data);

  for (const auto& holder : delivery->self->clients_) {
    PreviewRoutes::Client& client = *holder;

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

void DestroyDelivery(gpointer user_data) {
  auto* delivery = static_cast<Delivery*>(user_data);
  g_bytes_unref(delivery->part);
  delete delivery;
}

void HandleJpg(SoupServer*, SoupServerMessage* message, const char*,
               GHashTable*, gpointer user_data) {
  auto& self = *static_cast<PreviewRoutes*>(user_data);

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

void HandleMjpeg(SoupServer*, SoupServerMessage* message, const char*,
                 GHashTable*, gpointer user_data) {
  auto& self = *static_cast<PreviewRoutes*>(user_data);

  if (self.clients_.size() >= kMaxClients) {
    soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE,
                                   nullptr);
    return;
  }

  auto client = std::make_unique<PreviewRoutes::Client>();
  client->owner = &self;
  client->message = message;

  auto* raw_client = client.get();
  self.clients_.push_back(std::move(client));

  // The first client starts the JPEG encoder.
  if (self.clients_.size() == 1) {
    WEB_LOG(LogLevel::kInfo, "Started serving MJPEG");
    if (self.activation_) {
      self.activation_(true);
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

}  // namespace

namespace subtitler {

PreviewRoutes::Client::~Client() {
  if (pending != nullptr) {
    g_bytes_unref(pending);
  }
}

PreviewRoutes::PreviewRoutes(PreviewFrameBuffer& frames,
                             std::function<void(bool)> activation)
    : frames_{frames}, activation_{std::move(activation)} {}

PreviewRoutes::~PreviewRoutes() { Stop(); }

void PreviewRoutes::Register(SoupServer* server) {
  soup_server_add_handler(server, "/api/preview.jpg", HandleJpg, this,
                          nullptr);
  soup_server_add_handler(server, "/api/preview.mjpeg", HandleMjpeg, this,
                          nullptr);
}

void PreviewRoutes::Start(GMainContext* context) {
  context_ = context;
  watcher_thread_ = std::jthread{[this](std::stop_token stop) {
    // Producer side: wakes on every newly stored frame and hands its
    // multipart part to the io thread.
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
  }};
}

void PreviewRoutes::Stop() {
  // Reachable twice (WebServer teardown, then this destructor) and with
  // Start never called; join() on a non-joinable thread throws.
  if (!watcher_thread_.joinable()) {
    return;
  }

  watcher_thread_.request_stop();
  watcher_thread_.join();
}

}  // namespace subtitler
