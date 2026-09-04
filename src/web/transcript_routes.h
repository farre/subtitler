#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _GMainContext GMainContext;
typedef struct _SoupServer SoupServer;
typedef struct _SoupServerMessage SoupServerMessage;

namespace subtitler {

// The whisper transcript capture endpoint: GET /api/whisper/transcript
// is a server-sent-events stream carrying one event per transcribed
// whisper window, `data: {"timestamp_ns":N,"text":"..."}`. The web
// interface's dev mode appends them to its capture log, which
// subtitler-test's --windows file reads back verbatim. Publish is
// push-driven from the stream's whisper thread (posted onto the server
// context), so unlike the preview there is no watcher thread.
// SoupServerMessage objects are only ever touched on that context.
// Internal to the web module; web_server.cpp composes the route
// modules.
struct TranscriptRoutes {
  // One SSE client. All fields are touched only on the io thread.
  struct Client {
    TranscriptRoutes* owner;
    SoupServerMessage* message;  // borrowed; valid until "finished"

    // At most one event is in flight; the rest queue. A capture log
    // with holes is useless, so events are never dropped while the
    // queue is below kMaxPending.
    bool in_flight = false;
    std::deque<std::string> pending;
  };

  // Adds the route handler with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);
  // The context deliveries are posted to. Called on the io thread.
  void Start(GMainContext* context);
  // Queues one window's event to every connected client. Safe to call
  // from any thread; cheap when nobody is connected.
  void Publish(std::string text, std::int64_t timestamp_ns);

  GMainContext* context_ = nullptr;  // borrowed from the server
  std::vector<std::unique_ptr<Client>> clients_;  // io thread only
};

}  // namespace subtitler
