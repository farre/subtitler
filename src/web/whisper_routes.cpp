#include "web/whisper_routes.h"

#include <libsoup/soup.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "utils/paths.h"
#include "web/json_helpers.h"
#include "web/upload_stream.h"

namespace {

using namespace subtitler;

constexpr std::string_view kWhisperRoute = "/api/whisper";
constexpr std::string_view kWhisperModelsPrefix = "/api/whisper/models/";
// The upload cap covers every ggml model that can plausibly run on the
// target (small.en is ~460 MiB); the body streams into a staged file
// (#8), so it never sits in memory.
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

// PUT /api/whisper/models/<name>: stores the ggml model streamed into
// a staged file (see HandleModelStoreEarly) into the model store with
// a rename — the 512 MiB cap is an acceptance limit, the staged file
// is the receive buffer, so memory stays bounded throughout. libsoup
// hands the path over already percent-decoded, so no further
// unescaping happens here.
void HandleModelStore(SoupServerMessage* message, const char* path,
                      const WhisperRoutes& self) {
  const auto target =
      WhisperModelPath(*self.state_dir_, path + kWhisperModelsPrefix.size());
  if (!target) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  auto* upload = GetStagedUpload(message);
  if (upload == nullptr || upload->failed) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }
  if (upload->overflow) {
    soup_server_message_set_status(
        message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, nullptr);
    return;
  }
  if (upload->bytes == 0) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  // Verify completion before the rename: a partial model is never
  // exposed as installed.
  if (!upload->Close()) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }

  std::error_code error;
  std::filesystem::rename(upload->path, *target, error);
  if (error) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }
  upload->consumed = true;

  RespondJson(message, std::format("{{\"stored_name\":\"{}\"}}",
                                   JsonEscape(target->filename().string())));
  soup_server_message_set_status(message, SOUP_STATUS_CREATED, nullptr);
}

// Early handler for PUT /api/whisper/models/<name> (#8): sets up
// bounded streaming reception of the body into a staged file inside
// the model store (same filesystem, so the completion rename is
// atomic).
void HandleModelStoreEarly(SoupServer*, SoupServerMessage* message,
                           const char* path, GHashTable*,
                           gpointer user_data) {
  auto& self = *static_cast<WhisperRoutes*>(user_data);
  const std::string_view method{soup_server_message_get_method(message)};
  const std::string_view route{path};

  if (method != "PUT" || !route.starts_with(kWhisperModelsPrefix) ||
      route.size() <= kWhisperModelsPrefix.size()) {
    return;
  }

  BeginStagedUpload(message, *self.state_dir_ / "models", kMaxModelBytes);
}

// DELETE /api/whisper/models/<name>: removes the model from the store.
// The model the tap is currently running is in use and stays (409); a
// selected-but-disabled one can go — the store doesn't track the
// selection.
void HandleModelRemove(SoupServerMessage* message, const char* path,
                       const WhisperRoutes& self) {
  const std::string_view name{path + kWhisperModelsPrefix.size()};
  if (!WhisperModelPath(*self.state_dir_, name)) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  std::optional<WhisperRouteState> state;
  if (self.state_get_) {
    state = self.state_get_();
    if (state->enabled && state->model == name) {
      RespondJson(message, R"({"reason":"model in use"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      return;
    }
  }

  if (!RemoveWhisperModel(*self.state_dir_, name)) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  // Deleting the selected model clears the selection server-side: the
  // gone file must not linger as the selected (and persisted) model,
  // regardless of any browser follow-up.
  if (state && state->model == name && self.model_clear_) {
    self.model_clear_();
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
    if (state_dir_) {
      soup_server_add_early_handler(server, "/api/whisper",
                                    HandleModelStoreEarly, this, nullptr);
    }
    soup_server_add_handler(server, "/api/whisper", HandleWhisper, this,
                            nullptr);
  }
}

}  // namespace subtitler
