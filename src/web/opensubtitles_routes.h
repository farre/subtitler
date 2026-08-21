#pragma once

#include <optional>
#include <string>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServer SoupServer;

namespace subtitler {

// GET /api/opensubtitles answers the OpenSubtitles API key as JSON.
// An unset key disables the endpoint. Internal to the web module;
// web_server.cpp composes the route modules.
struct OpenSubtitlesRoutes {
  // Adds the route handler with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  std::optional<std::string> api_key_;
};

}  // namespace subtitler
