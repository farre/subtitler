#include "web/subtitle_routes.h"

#include <libsoup/soup.h>

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "utils/validation.h"
#include "web/json_helpers.h"
#include "web/upload_stream.h"

namespace {

using namespace subtitler;

constexpr std::string_view kSubtitlesRoute = "/api/subtitles";
constexpr std::string_view kSubtitlesPrefix = "/api/subtitles/";
constexpr std::string_view kSubtitleStateRoute = "/api/subtitle-state";
constexpr std::string_view kSubtitleSyncRoute = "/api/subtitle-sync";
constexpr std::size_t kMaxSubtitleBytes = 8 * 1024 * 1024;

void RespondPersistenceFailure(SoupServerMessage* message) {
  RespondJson(
      message,
      R"({"reason":"change applied, but persistence failed","applied":true,"persisted":false})");
  soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                 nullptr);
}

void RespondSubtitleState(SoupServerMessage* message,
                          const SubtitleState& state) {
  const std::string file = state.file.has_value()
                               ? std::format("\"{}\"", JsonEscape(*state.file))
                               : "null";
  const std::string font_family =
      state.font_family.has_value()
          ? std::format("\"{}\"", JsonEscape(*state.font_family))
          : "null";
  const std::string font_size =
      state.font_size.has_value() ? std::to_string(*state.font_size) : "null";
  const std::string font_color =
      state.font_color.has_value()
          ? std::format("\"#{:06x}\"", *state.font_color & 0xFF'FF'FF)
          : "null";
  RespondJson(
      message,
      std::format("{{\"file\":{},\"visible\":{},\"paused\":{},\"time\":{},"
                  "\"delay\":{},\"font_family\":{},\"font_size\":{},"
                  "\"font_color\":{}}}",
                  file, state.visible, state.paused, state.time_ms,
                  state.delay_ms, font_family, font_size, font_color));
}

// PUT /api/subtitles/<title> (#212): stores and activates the SRT in
// the body. The body streamed into a staged file (see
// HandleSubtitleUploadEarly), so this runs with bounded memory even
// for the largest accepted SRT. The title is whatever follows the
// prefix: libsoup hands handler paths over already percent-decoded (no
// raw-paths), so no further unescaping happens here — encoding and
// decoding each happen exactly once.
void HandleSubtitleUpload(SoupServerMessage* message, const char* path,
                          const SubtitleRoutes& self) {
  const std::string_view title{path + kSubtitlesPrefix.size()};

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
  if (!upload->Close()) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }

  // The cap keeps the read-back small enough to hold in memory.
  std::ifstream file{upload->path, std::ios::binary};
  const std::string contents{std::istreambuf_iterator<char>{file},
                             std::istreambuf_iterator<char>{}};
  if (file.bad()) {
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return;
  }

  const auto result = self.upload_(title, contents);

  switch (result.status) {
    case SubtitleUploadStatus::kStored:
      RespondJson(message, std::format("{{\"stored_name\":\"{}\"}}",
                                       JsonEscape(result.stored_name)));
      soup_server_message_set_status(message, SOUP_STATUS_CREATED, nullptr);
      break;
    case SubtitleUploadStatus::kInvalidTitle:
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
      break;
    case SubtitleUploadStatus::kFailed:
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     nullptr);
      break;
    case SubtitleUploadStatus::kPersistenceFailed:
      RespondPersistenceFailure(message);
      break;
  }
}

// GET /api/subtitles/<title>: the stored SRT as JSON.
void HandleSubtitleGet(SoupServerMessage* message, const char* path,
                       const SubtitleRoutes& self) {
  const auto body = self.get_(path + kSubtitlesPrefix.size());
  if (!body) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  RespondJson(message, std::format("{{\"body\":\"{}\"}}", JsonEscape(*body)));
}

// DELETE /api/subtitles/<title> (#453): removes the library entry.
void HandleSubtitleDelete(SoupServerMessage* message, const char* path,
                          const SubtitleRoutes& self) {
  switch (self.delete_(path + kSubtitlesPrefix.size())) {
    case SubtitleDeleteStatus::kDeleted:
      soup_server_message_set_status(message, SOUP_STATUS_NO_CONTENT, nullptr);
      break;
    case SubtitleDeleteStatus::kNotFound:
      soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
      break;
    case SubtitleDeleteStatus::kFailed:
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     nullptr);
      break;
    case SubtitleDeleteStatus::kPersistenceFailed:
      RespondPersistenceFailure(message);
      break;
  }
}

// Early handler for PUT /api/subtitles/<title> (#8): sets up bounded
// streaming reception of the body into a staged file; the completion
// handler (HandleSubtitleUpload) works with that file.
void HandleSubtitleUploadEarly(SoupServer*, SoupServerMessage* message,
                               const char* path, GHashTable*, gpointer) {
  const std::string_view method{soup_server_message_get_method(message)};
  const std::string_view route{path};

  if (method != "PUT" || !route.starts_with(kSubtitlesPrefix) ||
      route.size() <= kSubtitlesPrefix.size()) {
    return;
  }

  BeginStagedUpload(message, std::filesystem::temp_directory_path(),
                    kMaxSubtitleBytes);
}

void HandleSubtitles(SoupServer*, SoupServerMessage* message, const char* path,
                     GHashTable*, gpointer user_data) {
  auto& self = *static_cast<SubtitleRoutes*>(user_data);
  const std::string_view method{soup_server_message_get_method(message)};
  const std::string_view route{path};

  // GET on the collection lists the library titles (#441).
  if (method == "GET" && route == kSubtitlesRoute && self.list_) {
    RespondStringList(message, self.list_());
    return;
  }

  // The item routes are prefix-matched; the title is whatever follows
  // the prefix.
  const bool item = route.starts_with(kSubtitlesPrefix) &&
                    route.size() > kSubtitlesPrefix.size();

  if (item && method == "GET" && self.get_) {
    HandleSubtitleGet(message, path, self);
    return;
  }

  if (item && method == "PUT" && self.upload_) {
    HandleSubtitleUpload(message, path, self);
    return;
  }

  if (item && method == "DELETE" && self.delete_) {
    HandleSubtitleDelete(message, path, self);
    return;
  }

  // PUT and DELETE need a title.
  if ((method == "PUT" || method == "DELETE") && !item) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  // Anything else is a method the route doesn't support; Allow lists
  // the enabled ones.
  std::string allow;
  const auto extend = [&allow](std::string_view name) {
    if (!allow.empty()) {
      allow += ", ";
    }
    allow += name;
  };
  if (self.list_ || self.get_) {
    extend("GET");
  }
  if (self.upload_) {
    extend("PUT");
  }
  if (self.delete_) {
    extend("DELETE");
  }
  soup_message_headers_replace(
      soup_server_message_get_response_headers(message), "Allow",
      allow.c_str());
  soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                 nullptr);
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
        soup_server_message_get_response_headers(message), "Allow", "GET, PUT");
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
      const auto [end, error] =
          std::from_chars(param.data(), param.data() + param.size(), parsed);
      if (error != std::errc{} || end != param.data() + param.size()) {
        valid = false;
        break;
      }
      // Range-checked (#446): the stream converts ms to ns and composes
      // anchors; out-of-range values could overflow that arithmetic.
      if (name == "time") {
        if (!SubtitleOffsetMsValid(parsed)) {
          valid = false;
          break;
        }
        patch.time_ms = parsed;
      } else if (name == "delay") {
        if (!SubtitleOffsetMsValid(parsed)) {
          valid = false;
          break;
        }
        patch.delay_ms = parsed;
      } else if (SubtitleFontSizeValid(parsed)) {
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

  if (!valid) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  switch (self.state_set_(patch)) {
    case SubtitleStateSetStatus::kInvalid:
      soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
      return;
    case SubtitleStateSetStatus::kFailed:
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     nullptr);
      return;
    case SubtitleStateSetStatus::kPersistenceFailed:
      RespondPersistenceFailure(message);
      return;
    case SubtitleStateSetStatus::kApplied:
      break;
  }

  RespondSubtitleState(message, self.state_get_());
}

std::string_view SyncStatusName(SubtitleSyncStatus status) {
  switch (status) {
    case SubtitleSyncStatus::kIdle:
      return "idle";
    case SubtitleSyncStatus::kListening:
      return "listening";
    case SubtitleSyncStatus::kSynced:
      return "synced";
    case SubtitleSyncStatus::kFailed:
      return "failed";
  }
  return "idle";
}

void RespondSubtitleSync(SoupServerMessage* message,
                         const SubtitleSyncState& state) {
  std::string extra;
  if (state.time_ms) {
    extra += std::format(",\"time\":{}", *state.time_ms);
  }
  if (state.reason) {
    extra += std::format(",\"reason\":\"{}\"", JsonEscape(*state.reason));
  }
  RespondJson(message, std::format("{{\"state\":\"{}\"{}}}",
                                   SyncStatusName(state.status), extra));
}

// GET/PUT /api/subtitle-sync (#433): PUT starts (or restarts) the
// one-shot listening session — ?model=<name> picks the model the
// session enables the tap with when the tap is off; GET answers where
// it got to.
void HandleSubtitleSync(SoupServer*, SoupServerMessage* message,
                        const char* path, GHashTable* query,
                        gpointer user_data) {
  auto& self = *static_cast<SubtitleRoutes*>(user_data);
  const std::string_view method{soup_server_message_get_method(message)};

  if (std::string_view{path} != kSubtitleSyncRoute) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
    return;
  }

  if (method == "GET") {
    RespondSubtitleSync(message, self.sync_get_());
    return;
  }

  if (method != "PUT") {
    soup_message_headers_replace(
        soup_server_message_get_response_headers(message), "Allow", "GET, PUT");
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   nullptr);
    return;
  }

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

    if (name == "model" && !param.empty()) {
      model = std::string{param};
    } else {
      valid = false;
      break;
    }
  }

  if (!valid) {
    soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, nullptr);
    return;
  }

  switch (self.sync_start_(model ? std::make_optional<std::string_view>(*model)
                                 : std::nullopt)) {
    case SubtitleSyncStartResult::kStarted:
      RespondJson(message, R"({"state":"listening"})");
      soup_server_message_set_status(message, SOUP_STATUS_ACCEPTED, nullptr);
      break;
    case SubtitleSyncStartResult::kNoSubtitles:
      RespondJson(message,
                  R"({"state":"failed","reason":"no subtitles attached"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      break;
    case SubtitleSyncStartResult::kNoCapture:
      RespondJson(message,
                  R"({"state":"failed","reason":"capture isn't running"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      break;
    case SubtitleSyncStartResult::kNoWhisper:
      RespondJson(message,
                  R"({"state":"failed","reason":"whisper is disabled"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      break;
    case SubtitleSyncStartResult::kModelUnavailable:
      RespondJson(message,
                  R"({"state":"failed","reason":"the model isn't available"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      break;
    case SubtitleSyncStartResult::kUnparseableSubtitles:
      RespondJson(
          message,
          R"({"state":"failed","reason":"the subtitle file can't be parsed"})");
      soup_server_message_set_status(message, SOUP_STATUS_CONFLICT, nullptr);
      break;
  }
}

}  // namespace

namespace subtitler {

void SubtitleRoutes::Register(SoupServer* server) {
  if (upload_ || list_ || get_ || delete_) {
    if (upload_) {
      soup_server_add_early_handler(server, "/api/subtitles",
                                    HandleSubtitleUploadEarly, this, nullptr);
    }
    soup_server_add_handler(server, "/api/subtitles", HandleSubtitles, this,
                            nullptr);
  }
  if (state_get_ && state_set_) {
    soup_server_add_handler(server, "/api/subtitle-state", HandleSubtitleState,
                            this, nullptr);
  }
  if (sync_get_ && sync_start_) {
    soup_server_add_handler(server, "/api/subtitle-sync", HandleSubtitleSync,
                            this, nullptr);
  }
}

}  // namespace subtitler
