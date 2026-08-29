#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServer SoupServer;

namespace subtitler {

// The whisper tap's live state, exposed at GET /api/whisper (#19).
struct WhisperRouteState {
  bool enabled = false;
  // The model file name in use, when one is.
  std::optional<std::string> model;
};

// The whisper state getter/setter. The setter takes the changes as
// optionals (an unset field keeps its current value) and answers false
// on an unusable value — enabling with no model, or a model not in the
// store — mapped to 400.
using WhisperStateGetHandler = std::function<WhisperRouteState()>;
using WhisperStateSetHandler = std::function<bool(
    std::optional<bool> enabled, std::optional<std::string_view> model)>;

// The /api/whisper endpoints (#19): GET/PUT /api/whisper reads and
// changes the tap's state, and PUT /api/whisper/models/<name> stores a
// model into <state-dir>/models — models are never downloaded
// automatically; the web interface fetches them itself (from
// HuggingFace, which allows CORS) and stores them here. Model listing
// and storage resolve through state_dir_ via paths.h. Unset state
// hooks disable the state endpoint; an unset state_dir_ disables
// everything model-related.
struct WhisperRoutes {
  // Adds the route handlers with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  WhisperStateGetHandler state_get_;
  WhisperStateSetHandler state_set_;
  std::optional<std::filesystem::path> state_dir_;
};

}  // namespace subtitler
