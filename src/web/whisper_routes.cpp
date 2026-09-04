#include "web/whisper_routes.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "utils/paths.h"
#include "utils/unique_ptr.h"
#include "web/json_helpers.h"

namespace {

using namespace subtitler;

constexpr std::string_view kWhisperRoute = "/api/whisper";
constexpr std::string_view kWhisperModelsPrefix = "/api/whisper/models/";
// libsoup hands the handler the fully read body, so an uploaded model
// sits in memory once; the cap keeps that transient sane on the
// appliance and covers every ggml model that can plausibly run on the
// target (small.en is ~460 MiB).
constexpr std::size_t kMaxModelBytes = 512 * 1024 * 1024;

std::string StringArrayJson(const std::vector<std::string>& entries) {
  std::string json{"["};
  std::string_view separator;
  for (const auto& entry : entries) {
    json += std::format("{}\"{}\"", separator, JsonEscape(entry));
    separator = ",";
  }
  json += "]";
  return json;
}

void RespondWhisperState(
    SoupServerMessage* message, const WhisperRouteState& state,
    const std::optional<std::filesystem::path>& state_dir) {
  const std::string model =
      state.model.has_value() ? std::format("\"{}\"", JsonEscape(*state.model))
                              : "null";
  const std::string models =
      state_dir ? StringArrayJson(ListWhisperModels(*state_dir)) : "[]";

  RespondJson(message, std::format("{{\"enabled\":{},\"model\":{},"
                                   "\"models\":{}}}",
                                   state.enabled, model, models));
}

// PUT /api/whisper/models/<name>: stores the ggml model from the body
// into the model store (temp file + rename). libsoup invokes the
// handler after the request body is complete.
void HandleModelStore(SoupServerMessage* message, const char* path,
                      const WhisperRoutes& self) {
  const UniquePtr<gchar, g_free> decoded{
      g_uri_unescape_string(path + kWhisperModelsPrefix.size(), nullptr)};

  const auto target = decoded != nullptr
                          ? WhisperModelPath(*self.state_dir_, decoded.get())
                          : std::nullopt;
  if (!target) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  const UniquePtr<GBytes, g_bytes_unref> body{
      soup_message_body_flatten(soup_server_message_get_request_body(message))};

  const gsize body_size = body != nullptr ? g_bytes_get_size(body.get()) : 0;
  if (body_size == 0) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  if (body_size > kMaxModelBytes) {
    soup_server_message_set_status(
        message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, nullptr);
    return;
  }

  gsize data_size;
  const auto* data =
      static_cast<const char*>(g_bytes_get_data(body.get(), &data_size));

  std::error_code error;
  std::filesystem::create_directories(target->parent_path(), error);
  if (error) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }

  std::filesystem::path temp = *target;
  temp += ".part";
  {
    std::ofstream file{temp, std::ios::binary | std::ios::trunc};
    file.write(data, static_cast<std::streamsize>(data_size));
    if (!file) {
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     nullptr);
      return;
    }
  }

  std::filesystem::rename(temp, *target, error);
  if (error) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }

  RespondJson(message, std::format("{{\"stored_name\":\"{}\"}}",
                                   JsonEscape(target->filename().string())));
  soup_server_message_set_status(message, SOUP_STATUS_CREATED, nullptr);
}

// DELETE /api/whisper/models/<name>: removes the model from the store.
// The model the tap is currently running is in use and stays (409); a
// selected-but-disabled one can go — the store doesn't track the
// selection.
void HandleModelRemove(SoupServerMessage* message, const char* path,
                       const WhisperRoutes& self) {
  const UniquePtr<gchar, g_free> decoded{
      g_uri_unescape_string(path + kWhisperModelsPrefix.size(), nullptr)};
  if (decoded == nullptr ||
      !WhisperModelPath(*self.state_dir_, decoded.get())) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  if (self.state_get_) {
    const auto state = self.state_get_();
    if (state.enabled && state.model == decoded.get()) {
      RespondJson(message, R"({"reason":"model in use"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      return;
    }
  }

  if (!RemoveWhisperModel(*self.state_dir_, decoded.get())) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  soup_server_message_set_status(message, SOUP_STATUS_NO_CONTENT, nullptr);
}

void HandleWhisper(SoupServer*, SoupServerMessage* message, const char* path,
                   GHashTable* query, gpointer user_data) {
  auto& self = *static_cast<WhisperRoutes*>(user_data);
  const std::string_view method{soup_server_message_get_method(message)};
  const std::string_view route{path};

  if (route == kWhisperRoute && self.state_get_ && self.state_set_) {
    if (method == "GET") {
      RespondWhisperState(message, self.state_get_(), self.state_dir_);
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

    std::optional<bool> enabled;
    std::optional<std::string> model;
    bool valid = true;

    GHashTableIter iter;
    gpointer key, value;
    if (query != nullptr) {
      g_hash_table_iter_init(&iter, query);
    }

    while (query != nullptr && g_hash_table_iter_next(&iter, &key, &value)) {
      const std::string_view name{static_cast<const char*>(key)};
      const std::string_view param{static_cast<const char*>(value)};

      if (name == "enabled") {
        if (param != "true" && param != "false") {
          valid = false;
          break;
        }
        enabled = param == "true";
      } else if (name == "model") {
        if (!WhisperModelNameValid(param)) {
          valid = false;
          break;
        }
        model = std::string{param};
      } else {
        valid = false;
        break;
      }
    }

    if (!valid ||
        !self.state_set_(enabled,
                         model ? std::make_optional<std::string_view>(*model)
                               : std::nullopt)) {
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
      return;
    }

    RespondWhisperState(message, self.state_get_(), self.state_dir_);
    return;
  }

  if (method == "PUT" && self.state_dir_ &&
      route.starts_with(kWhisperModelsPrefix) &&
      route.size() > kWhisperModelsPrefix.size()) {
    HandleModelStore(message, path, self);
    return;
  }

  if (method == "DELETE" && self.state_dir_ &&
      route.starts_with(kWhisperModelsPrefix) &&
      route.size() > kWhisperModelsPrefix.size()) {
    HandleModelRemove(message, path, self);
    return;
  }

  soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
}

}  // namespace

namespace subtitler {

void WhisperRoutes::Register(SoupServer* server) {
  if ((state_get_ && state_set_) || state_dir_) {
    soup_server_add_handler(server, "/api/whisper", HandleWhisper, this,
                            nullptr);
  }
}

}  // namespace subtitler
