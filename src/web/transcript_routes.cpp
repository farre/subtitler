#include "web/transcript_routes.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "utils/logging.h"
#include "web/json_helpers.h"

namespace {

using namespace subtitler;

constexpr std::size_t kMaxClients = 4;
// Beyond this a stuck client loses its oldest queued events rather
// than growing the backlog without bound.
constexpr std::size_t kMaxPending = 256;

std::string MakeEvent(std::string_view text, std::int64_t timestamp_ns) {
  return std::format("data: {{\"timestamp_ns\":{},\"text\":\"{}\"}}\n\n",
                     timestamp_ns, JsonEscape(text));
}

void SendEvent(TranscriptRoutes::Client& client, const std::string& event) {
  client.in_flight = true;

  soup_message_body_append(soup_server_message_get_response_body(client.message),
                           SOUP_MEMORY_COPY, event.data(), event.size());
  soup_server_message_unpause(client.message);
}

void OnWroteChunk(SoupServerMessage*, gpointer user_data) {
  auto& client = *static_cast<TranscriptRoutes::Client*>(user_data);
  client.in_flight = false;

  if (client.pending.empty()) {
    return;
  }

  const std::string next = std::move(client.pending.front());
  client.pending.pop_front();
  SendEvent(client, next);
}

void OnFinished(SoupServerMessage*, gpointer user_data) {
  auto* client = static_cast<TranscriptRoutes::Client*>(user_data);
  auto& self = *client->owner;

  WEB_LOG(LogLevel::kInfo, "Stopped serving the transcript stream");
  std::erase_if(self.clients_,
                [client](const auto& holder) { return holder.get() == client; });
}

struct Delivery {
  TranscriptRoutes* self;
  std::string event;
};

gboolean Deliver(gpointer user_data) {
  auto* delivery = static_cast<Delivery*>(user_data);

  for (const auto& holder : delivery->self->clients_) {
    TranscriptRoutes::Client& client = *holder;

    if (client.in_flight) {
      if (client.pending.size() >= kMaxPending) {
        client.pending.pop_front();
      }
      client.pending.push_back(delivery->event);
    } else {
      SendEvent(client, delivery->event);
    }
  }

  return G_SOURCE_REMOVE;
}

void DestroyDelivery(gpointer user_data) {
  delete static_cast<Delivery*>(user_data);
}

void HandleTranscript(SoupServer*, SoupServerMessage* message, const char*,
                      GHashTable*, gpointer user_data) {
  auto& self = *static_cast<TranscriptRoutes*>(user_data);

  if (self.clients_.size() >= kMaxClients) {
    soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE,
                                   nullptr);
    return;
  }

  auto client = std::make_unique<TranscriptRoutes::Client>();
  client->owner = &self;
  client->message = message;

  auto* raw_client = client.get();
  self.clients_.push_back(std::move(client));

  WEB_LOG(LogLevel::kInfo, "Started serving the transcript stream");

  g_signal_connect(message, "wrote-chunk", G_CALLBACK(OnWroteChunk),
                   raw_client);
  g_signal_connect(message, "finished", G_CALLBACK(OnFinished), raw_client);

  auto* headers = soup_server_message_get_response_headers(message);
  soup_message_headers_replace(headers, "Content-Type", "text/event-stream");
  soup_message_headers_replace(headers, "Cache-Control", "no-store");

  // HTTP transfer framing. Written chunks are discarded, so an
  // indefinite response cannot grow.
  soup_message_headers_set_encoding(headers, SOUP_ENCODING_CHUNKED);
  soup_message_body_set_accumulate(
      soup_server_message_get_response_body(message), FALSE);

  soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);

  // No explicit pause: libsoup pauses a chunked response while no chunk
  // is available. An initial comment line opens the stream right away,
  // so a client knows it's connected before the first window arrives.
  SendEvent(*raw_client, ":\n\n");
}

}  // namespace

namespace subtitler {

void TranscriptRoutes::Register(SoupServer* server) {
  soup_server_add_handler(server, "/api/whisper/transcript",
                          HandleTranscript, this, nullptr);
}

void TranscriptRoutes::Start(GMainContext* context) {
  context_ = context;
}

void TranscriptRoutes::Publish(std::string text, std::int64_t timestamp_ns) {
  if (context_ == nullptr) {
    return;
  }

  auto* delivery = new Delivery{this, MakeEvent(text, timestamp_ns)};
  g_main_context_invoke_full(context_, G_PRIORITY_DEFAULT, Deliver, delivery,
                             DestroyDelivery);
}

}  // namespace subtitler
