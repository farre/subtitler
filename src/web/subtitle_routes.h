#pragma once

#include <functional>
#include <string>
#include <string_view>

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

// PUT /api/subtitles/<title> (#212): stores and activates the uploaded
// SRT. Without an upload handler the endpoint is not registered.
// Internal to the web module; web_server.cpp composes the route
// modules.
struct SubtitleRoutes {
  explicit SubtitleRoutes(SubtitleUploadHandler upload);

  // Adds the route handlers with this as user_data. Called on the io
  // thread.
  void Register(SoupServer* server);

  SubtitleUploadHandler upload_;
};

}  // namespace subtitler
