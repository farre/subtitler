#pragma once

#include <functional>
#include <string>
#include <vector>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServer SoupServer;

namespace subtitler {

// The renderer-available font families, for GET /api/fonts (#159).
using FontListHandler = std::function<std::vector<std::string>()>;

// GET /api/fonts lists the families the subtitle renderer can use
// (#159). An unset hook disables the endpoint. Internal to the web
// module; web_server.cpp composes the route modules.
struct FontRoutes {
  // Adds the route handler with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  FontListHandler list_;
};

}  // namespace subtitler
