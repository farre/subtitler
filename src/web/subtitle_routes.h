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
  // The cue color, big-endian ARGB (0xFFRRGGBB for opaque #RRGGBB);
  // nullopt keeps the renderer's default.
  std::optional<std::uint32_t> font_color;
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
  std::optional<std::uint32_t> font_color;
};

// The library titles, for GET /api/subtitles (#441).
using SubtitleListHandler = std::function<std::vector<std::string>()>;
// The stored SRT for a library title, for GET /api/subtitles/<title>;
// nullopt when the title isn't in the library, mapped to 404.
using SubtitleGetHandler =
    std::function<std::optional<std::string>(std::string_view title)>;

// The result of removing a library entry (#453).
enum class SubtitleDeleteStatus : std::uint8_t {
  kDeleted,
  kNotFound,  // no such library title
  kFailed,    // removal or live detach failed
};

// Removes a library title, for DELETE /api/subtitles/<title>.
using SubtitleDeleteHandler =
    std::function<SubtitleDeleteStatus(std::string_view title)>;
// The live subtitle state getters/setters; the setter answers false on
// an unusable value (e.g. a title not in the library), mapped to 400.
using SubtitleStateGetHandler = std::function<SubtitleState()>;
using SubtitleStateSetHandler = std::function<bool(const SubtitleStatePatch&)>;

// The one-shot auto-sync state, exposed at GET /api/subtitle-sync
// (#433).
enum class SubtitleSyncStatus : std::uint8_t {
  kIdle,
  kListening,
  kSynced,
  kFailed,
};

struct SubtitleSyncState {
  SubtitleSyncStatus status = SubtitleSyncStatus::kIdle;
  // The matched SRT position in ms; set when synced.
  std::optional<std::int64_t> time_ms;
  // Why the session failed; set when failed.
  std::optional<std::string> reason;
};

// PUT /api/subtitle-sync's outcome; the non-started answers map to 409
// with an explanatory body.
enum class SubtitleSyncStartResult : std::uint8_t {
  kStarted,
  kNoSubtitles,
  kNoWhisper,
  kUnparseableSubtitles,
};

using SubtitleSyncGetHandler = std::function<SubtitleSyncState()>;
using SubtitleSyncStartHandler = std::function<SubtitleSyncStartResult()>;

// The /api/subtitles endpoints: PUT /api/subtitles/<title> uploads
// (#212), GET /api/subtitles/<title> answers the stored SRT, DELETE
// /api/subtitles/<title> removes it (#453), GET /api/subtitles lists
// the library, GET/PUT /api/subtitle-state reads and changes the live
// state (#441), and GET/PUT /api/subtitle-sync runs the one-shot
// auto-sync (#433). An unset hook disables its endpoint. Internal to
// the web module; web_server.cpp composes the route modules.
struct SubtitleRoutes {
  // Adds the route handlers with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  SubtitleUploadHandler upload_;
  SubtitleListHandler list_;
  SubtitleGetHandler get_;
  SubtitleDeleteHandler delete_;
  SubtitleStateGetHandler state_get_;
  SubtitleStateSetHandler state_set_;
  SubtitleSyncGetHandler sync_get_;
  SubtitleSyncStartHandler sync_start_;
};

}  // namespace subtitler
