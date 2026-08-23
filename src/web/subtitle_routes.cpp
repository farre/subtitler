#include "web/subtitle_routes.h"

#include <libsoup/soup.h>

#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "utils/unique_ptr.h"
#include "web/json_helpers.h"

namespace {

using namespace subtitler;

constexpr std::string_view kSubtitlesRoute = "/api/subtitles";
constexpr std::string_view kSubtitlesPrefix = "/api/subtitles/";
constexpr std::string_view kSubtitleStateRoute = "/api/subtitle-state";
constexpr std::size_t kMaxSubtitleBytes = 8 * 1024 * 1024;

void RespondSubtitleState(SoupServerMessage* message,
                          const SubtitleState& state) {
  const std::string file = state.file.has_value()
                               ? std::format("\"{}\"", JsonEscape(*state.file))
                               : "null";
  const std::string font_family =
      state.font_family.has_value()
          ? std::format("\"{}\"", JsonEscape(*state.font_family))
          : "null";
  const std::string font_size = state.font_size.has_value()
                                    ? std::to_string(*state.font_size)
                                    : "null";
  const std::string font_color =
      state.font_color.has_value()
          ? std::format("\"#{:06x}\"", *state.font_color & 0xFF'FF'FF)
          : "null";
  RespondJson(message,
              std::format(
                  "{{\"file\":{},\"visible\":{},\"paused\":{},\"time\":{},"
                  "\"delay\":{},\"font_family\":{},\"font_size\":{},"
                  "\"font_color\":{}}}",
                  file, state.visible, state.paused, state.time_ms,
                  state.delay_ms, font_family, font_size, font_color));
}

// PUT /api/subtitles/<title> (#212): stores and activates the SRT in
// the body. libsoup invokes the handler after the request body is
// complete, so the body is fully available here.
void HandleSubtitleUpload(SoupServerMessage* message, const char* path,
                          const SubtitleRoutes& self) {
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
      RespondJson(message,
                  std::format("{{\"stored_name\":\"{}\"}}",
                              JsonEscape(result.stored_name)));
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

void HandleSubtitles(SoupServer*, SoupServerMessage* message,
                     const char* path, GHashTable*, gpointer user_data) {
  auto& self = *static_cast<SubtitleRoutes*>(user_data);
  const std::string_view method{soup_server_message_get_method(message)};
  const std::string_view route{path};

  // GET on the collection lists the library titles (#441).
  if (method == "GET" && route == kSubtitlesRoute && self.list_) {
    RespondStringList(message, self.list_());
    return;
  }

  if (!self.upload_) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  if (method != "PUT") {
    soup_message_headers_replace(
        soup_server_message_get_response_headers(message), "Allow", "PUT");
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   nullptr);
    return;
  }

  // The handler is prefix-matched; the title is whatever follows the
  // prefix, percent-encoded.
  if (!route.starts_with(kSubtitlesPrefix) ||
      route.size() == kSubtitlesPrefix.size()) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  HandleSubtitleUpload(message, path, self);
}

// GET/PUT /api/subtitle-state (#441). PUT takes its changes as query
// parameters — libsoup hands them over already parsed — and answers
// with the state after applying them.
void HandleSubtitleState(SoupServer*, SoupServerMessage* message,
                         const char* path, GHashTable* query,
                         gpointer user_data) {
  auto& self = *static_cast<SubtitleRoutes*>(user_data);
  const std::string_view method{soup_server_message_get_method(message)};

  if (std::string_view{path} != kSubtitleStateRoute) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  if (method == "GET") {
    RespondSubtitleState(message, self.state_get_());
    return;
  }

  if (method != "PUT") {
    soup_message_headers_replace(
        soup_server_message_get_response_headers(message), "Allow",
        "GET, PUT");
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   nullptr);
    return;
  }

  SubtitleStatePatch patch;
  bool valid = true;

  GHashTableIter iter;
  gpointer key, value;
  if (query != nullptr) {
    g_hash_table_iter_init(&iter, query);
  }

  while (query != nullptr && g_hash_table_iter_next(&iter, &key, &value)) {
    const std::string_view name{static_cast<const char*>(key)};
    const std::string_view param{static_cast<const char*>(value)};

    if (name == "file") {
      patch.file = std::string{param};
    } else if (name == "font_family") {
      if (param.empty()) {
        valid = false;
        break;
      }
      patch.font_family = std::string{param};
    } else if (name == "font_color") {
      // "#rrggbb"; the alpha is always opaque.
      if (param.size() != 7 || param.front() != '#') {
        valid = false;
        break;
      }
      std::uint32_t rgb;
      const auto [end, error] = std::from_chars(
          param.data() + 1, param.data() + param.size(), rgb, 16);
      if (error != std::errc{} || end != param.data() + param.size()) {
        valid = false;
        break;
      }
      patch.font_color = 0xFF'00'00'00 | rgb;
    } else if (name == "visible" || name == "paused") {
      if (param != "true" && param != "false") {
        valid = false;
        break;
      }
      if (name == "visible") {
        patch.visible = param == "true";
      } else {
        patch.paused = param == "true";
      }
    } else if (name == "time" || name == "delay" || name == "font_size") {
      std::int64_t parsed;
      const auto [end, error] = std::from_chars(
          param.data(), param.data() + param.size(), parsed);
      if (error != std::errc{} || end != param.data() + param.size()) {
        valid = false;
        break;
      }
      if (name == "time") {
        patch.time_ms = parsed;
      } else if (name == "delay") {
        patch.delay_ms = parsed;
      } else if (parsed > 0) {
        patch.font_size = parsed;
      } else {
        valid = false;
        break;
      }
    } else {
      valid = false;
      break;
    }
  }

  if (!valid || !self.state_set_(patch)) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  RespondSubtitleState(message, self.state_get_());
}

}  // namespace

namespace subtitler {

void SubtitleRoutes::Register(SoupServer* server) {
  if (upload_ || list_) {
    soup_server_add_handler(server, "/api/subtitles", HandleSubtitles, this,
                            nullptr);
  }
  if (state_get_ && state_set_) {
    soup_server_add_handler(server, "/api/subtitle-state",
                            HandleSubtitleState, this, nullptr);
  }
}

}  // namespace subtitler
