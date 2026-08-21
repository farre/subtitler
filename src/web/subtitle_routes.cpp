#include "web/subtitle_routes.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <string_view>

#include "utils/unique_ptr.h"

namespace {

using namespace subtitler;

constexpr std::string_view kSubtitlesPrefix = "/api/subtitles/";
constexpr std::size_t kMaxSubtitleBytes = 8 * 1024 * 1024;

// PUT /api/subtitles/<title> (#212): stores and activates the SRT in
// the body. libsoup invokes the handler after the request body is
// complete, so the body is fully available here.
void HandleSubtitleUpload(SoupServer*, SoupServerMessage* message,
                          const char* path, GHashTable*,
                          gpointer user_data) {
  auto& self = *static_cast<SubtitleRoutes*>(user_data);

  if (std::string_view{soup_server_message_get_method(message)} != "PUT") {
    soup_message_headers_replace(
        soup_server_message_get_response_headers(message), "Allow", "PUT");
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   nullptr);
    return;
  }

  // The handler is prefix-matched; the title is whatever follows the
  // prefix, percent-encoded.
  const std::string_view raw{path};
  if (!raw.starts_with(kSubtitlesPrefix) ||
      raw.size() == kSubtitlesPrefix.size()) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  const UniquePtr<gchar, g_free> decoded{
      g_uri_unescape_string(path + kSubtitlesPrefix.size(), nullptr)};
  if (decoded == nullptr) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  const UniquePtr<GBytes, g_bytes_unref> body{soup_message_body_flatten(
      soup_server_message_get_request_body(message))};

  const gsize body_size = body != nullptr ? g_bytes_get_size(body.get()) : 0;
  if (body_size == 0) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  if (body_size > kMaxSubtitleBytes) {
    soup_server_message_set_status(
        message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, nullptr);
    return;
  }

  gsize data_size;
  const auto* data =
      static_cast<const char*>(g_bytes_get_data(body.get(), &data_size));

  const auto result =
      self.upload_(decoded.get(), std::string_view{data, data_size});

  switch (result.status) {
    case SubtitleUploadStatus::kStored:
      soup_server_message_set_response(
          message, "text/plain; charset=utf-8", SOUP_MEMORY_COPY,
          result.stored_name.data(), result.stored_name.size());
      soup_server_message_set_status(message, SOUP_STATUS_CREATED, nullptr);
      break;
    case SubtitleUploadStatus::kInvalidTitle:
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST,
                                     nullptr);
      break;
    case SubtitleUploadStatus::kFailed:
      soup_server_message_set_status(
          message, SOUP_STATUS_INTERNAL_SERVER_ERROR, nullptr);
      break;
  }
}

}  // namespace

namespace subtitler {

SubtitleRoutes::SubtitleRoutes(SubtitleUploadHandler upload)
    : upload_{std::move(upload)} {}

void SubtitleRoutes::Register(SoupServer* server) {
  if (upload_) {
    soup_server_add_handler(server, "/api/subtitles", HandleSubtitleUpload,
                            this, nullptr);
  }
}

}  // namespace subtitler
