#include "web/opensubtitles_routes.h"

#include <libsoup/soup.h>

#include <format>
#include <string>
#include <string_view>

#include "web/json_helpers.h"

namespace {

using namespace subtitler;

constexpr std::string_view kOpenSubtitlesRoute = "/api/opensubtitles";

void HandleOpenSubtitles(SoupServer*, SoupServerMessage* message,
                         const char* path, GHashTable*, gpointer user_data) {
  auto& self = *static_cast<OpenSubtitlesRoutes*>(user_data);

  if (std::string_view{path} != kOpenSubtitlesRoute) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  if (std::string_view{soup_server_message_get_method(message)} != "GET") {
    soup_message_headers_replace(
        soup_server_message_get_response_headers(message), "Allow", "GET");
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   nullptr);
    return;
  }

  RespondJson(message, std::format("{{\"api_key\":\"{}\"}}",
                                   JsonEscape(*self.api_key_)));
}

}  // namespace

namespace subtitler {

void OpenSubtitlesRoutes::Register(SoupServer* server) {
  if (api_key_) {
    soup_server_add_handler(server, "/api/opensubtitles", HandleOpenSubtitles,
                            this, nullptr);
  }
}

}  // namespace subtitler
