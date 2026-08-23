#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServer SoupServer;

namespace subtitler {

// The result of handing an uploaded subtitle to the appliance (#212).
enum class SubtitleUploadStatus {
  kStored,        // saved to the library and activated on the stream
  kInvalidTitle,  // not a usable library name
  kFailed,        // storage or activation failed
};

struct SubtitleUploadResult {
  SubtitleUploadStatus status;
  // The library-relative name (e.g. "m/Movie.srt"); set when kStored.
  std::string stored_name;
};

// Handles an uploaded SRT on the server's io thread: title is the bare
// library filename, contents the raw SRT text.
using SubtitleUploadHandler = std::function<SubtitleUploadResult(
    std::string_view title, std::string_view contents)>;

// The live subtitle state, exposed at GET /api/subtitle-state (#441).
struct SubtitleState {
  std::optional<std::string> file;  // library title; nullopt when detached
  bool visible = true;
  bool paused = false;
  std::int64_t time_ms = 0;
  std::int64_t delay_ms = 0;
  // The cue font (#159): family and size in points; nullopt keeps the
  // renderer's default.
  std::optional<std::string> font_family;
  std::optional<std::int64_t> font_size;
};

// A subset of SubtitleState to change (PUT /api/subtitle-state). An
// empty file detaches subtitles.
struct SubtitleStatePatch {
  std::optional<std::string> file;
  std::optional<bool> visible;
  std::optional<bool> paused;
  std::optional<std::int64_t> time_ms;
  std::optional<std::int64_t> delay_ms;
  std::optional<std::string> font_family;
  std::optional<std::int64_t> font_size;
};

// The library titles, for GET /api/subtitles (#441).
using SubtitleListHandler = std::function<std::vector<std::string>()>;
// The live subtitle state getters/setters; the setter answers false on
// an unusable value (e.g. a title not in the library), mapped to 400.
using SubtitleStateGetHandler = std::function<SubtitleState()>;
using SubtitleStateSetHandler =
    std::function<bool(const SubtitleStatePatch&)>;

// The /api/subtitles endpoints: PUT /api/subtitles/<title> uploads
// (#212), GET /api/subtitles lists the library, GET/PUT
// /api/subtitle-state reads and changes the live state (#441). An unset
// hook disables its endpoint. Internal to the web module;
// web_server.cpp composes the route modules.
struct SubtitleRoutes {
  // Adds the route handlers with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  SubtitleUploadHandler upload_;
  SubtitleListHandler list_;
  SubtitleStateGetHandler state_get_;
  SubtitleStateSetHandler state_set_;
};

}  // namespace subtitler
