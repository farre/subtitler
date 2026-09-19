#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServerMessage SoupServerMessage;

namespace subtitler {

// Bounded request-body reception for uploads (#8): an early handler
// streams the body into a uniquely named staged file instead of
// letting libsoup accumulate the whole body in memory (the 512 MiB
// model cap and 8 MiB subtitle cap are acceptance limits, not receive
// buffers). The route's completion handler then works with the staged
// file. Declare-oversize bodies are rejected before reception; bodies
// crossing the cap while streaming stop at the cap.
struct StagedUpload {
  StagedUpload() = default;
  StagedUpload(StagedUpload&&) = default;
  StagedUpload& operator=(StagedUpload&&) = default;
  ~StagedUpload();

  // The staged file; removed on destruction unless consumed.
  std::filesystem::path path;
  std::ofstream file;
  std::uint64_t bytes = 0;
  std::uint64_t max_bytes = 0;
  // More than the cap arrived (the rest is discarded): map to 413.
  bool overflow = false;
  // Open/write/close failed: map to 500.
  bool failed = false;
  // The completion took the file (renamed it away).
  bool consumed = false;

  // Flushes and closes the staged file; false on failure.
  bool Close();
};

// Sets up streaming reception of message's body into staging_dir
// (created if needed), bounded at max_bytes. The returned upload is
// owned by the message; use GetStagedUpload in the completion handler.
// nullptr when the message was already answered (excessive declared
// length, too many concurrent uploads, no staging possible).
StagedUpload* BeginStagedUpload(SoupServerMessage* message,
                                const std::filesystem::path& staging_dir,
                                std::uint64_t max_bytes);

// The upload staged on this message; nullptr when none is.
StagedUpload* GetStagedUpload(SoupServerMessage* message);

}  // namespace subtitler
