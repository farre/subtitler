#include "web/json_helpers.h"

#include <libsoup/soup.h>

#include <format>

namespace subtitler {

std::string JsonEscape(std::string_view text) {
  std::string escaped;

  for (const char c : text) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          escaped += std::format("\\u{:04x}", c);
        } else {
          escaped += c;
        }
    }
  }

  return escaped;
}

void RespondJson(SoupServerMessage* message, const std::string& json) {
  soup_server_message_set_response(message, "application/json",
                                   SOUP_MEMORY_COPY, json.data(), json.size());
  soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
}

}  // namespace subtitler
