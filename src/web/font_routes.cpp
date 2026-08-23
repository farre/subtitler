#include "web/font_routes.h"

#include <libsoup/soup.h>

#include <string_view>

#include "web/json_helpers.h"

namespace {

using namespace subtitler;

constexpr std::string_view kFontsRoute = "/api/fonts";

// GET /api/fonts (#159): the renderer-available font families.
void HandleFonts(SoupServer*, SoupServerMessage* message, const char* path,
                 GHashTable*, gpointer user_data) {
  auto& self = *static_cast<FontRoutes*>(user_data);

  if (std::string_view{path} != kFontsRoute) {
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

  RespondStringList(message, self.list_());
}

}  // namespace

namespace subtitler {

void FontRoutes::Register(SoupServer* server) {
  if (list_) {
    soup_server_add_handler(server, "/api/fonts", HandleFonts, this, nullptr);
  }
}

}  // namespace subtitler
