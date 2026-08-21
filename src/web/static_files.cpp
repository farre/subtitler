#include "web/static_files.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include "utils/unique_ptr.h"

namespace {

using namespace subtitler;

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

void HandleStatic(SoupServer*, SoupServerMessage* message, const char* path,
                  GHashTable*, gpointer user_data) {
  auto& self = *static_cast<StaticFiles*>(user_data);

  const auto not_found = [&] {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
  };

  if (std::string_view{soup_server_message_get_method(message)} != "GET") {
    not_found();
    return;
  }

  const UniquePtr<gchar, g_free> decoded{g_uri_unescape_string(path, nullptr)};
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

}  // namespace

namespace subtitler {

StaticFiles::StaticFiles(std::optional<std::filesystem::path> web_root)
    : web_root_{std::move(web_root)} {}

void StaticFiles::Register(SoupServer* server) {
  if (web_root_) {
    soup_server_add_handler(server, "/", HandleStatic, this, nullptr);
  }
}

}  // namespace subtitler
