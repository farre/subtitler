#pragma once

#include <filesystem>
#include <optional>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServer SoupServer;

namespace subtitler {

// The static fallback (#212): GET paths not claimed by a registered
// route are served from the web root; "/" maps to index.html. Only
// .html/.js/.css/.png are served — the allowlist doubles as the MIME
// map — and anything else, non-GET methods, and traversal attempts are
// 404. Without a web root nothing is registered. Internal to the web
// module; web_server.cpp composes the route modules.
struct StaticFiles {
  explicit StaticFiles(std::optional<std::filesystem::path> web_root);

  // Adds the default handler with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  std::optional<std::filesystem::path> web_root_;
};

}  // namespace subtitler
