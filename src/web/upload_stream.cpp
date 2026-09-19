#include "web/upload_stream.h"

#include <libsoup/soup.h>

#include <atomic>
#include <format>
#include <memory>
#include <system_error>

#include "utils/logging.h"

namespace {

using namespace subtitler;

// Simultaneous streaming uploads beyond this are refused (503): memory
// is bounded by streaming, but every upload still costs a file and
// disk bandwidth on the appliance's io thread.
constexpr int kMaxConcurrentUploads = 4;
std::atomic_int uploads_in_flight = 0;
std::atomic_uint upload_counter = 0;

constexpr char kUploadKey[] = "subtitler-staged-upload";

void DeleteStagedUpload(gpointer data) {
  delete static_cast<StagedUpload*>(data);
  --uploads_in_flight;
}

void OnGotChunk(SoupServerMessage* message, GBytes* chunk, gpointer) {
  auto& upload = *static_cast<StagedUpload*>(
      g_object_get_data(G_OBJECT(message), kUploadKey));

  const gsize size = g_bytes_get_size(chunk);
  if (upload.overflow || upload.failed) {
    return;
  }
  if (upload.bytes + size > upload.max_bytes) {
    // The body crossed the cap mid-stream; discard the rest. The
    // completion handler answers 413.
    upload.overflow = true;
    return;
  }

  const auto* data =
      static_cast<const char*>(g_bytes_get_data(chunk, nullptr));
  upload.file.write(data, static_cast<std::streamsize>(size));
  if (!upload.file) {
    WEB_LOG(LogLevel::kWarning, "Upload to {} failed while writing",
            upload.path.string());
    upload.failed = true;
    return;
  }
  upload.bytes += size;
}

}  // namespace

namespace subtitler {

StagedUpload::~StagedUpload() {
  Close();
  if (!consumed) {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
}

bool StagedUpload::Close() {
  if (!file.is_open()) {
    return !failed;
  }
  file.close();
  if (file.fail()) {
    failed = true;
  }
  return !failed;
}

StagedUpload* BeginStagedUpload(SoupServerMessage* message,
                                const std::filesystem::path& staging_dir,
                                std::uint64_t max_bytes) {
  // Reject an excessive declared length before receiving anything:
  // nothing is staged, and with accumulation off the body is discarded
  // as it arrives (libsoup answers the 413 once the request completes;
  // a truncated request just gets its connection closed).
  const auto declared = soup_message_headers_get_content_length(
      soup_server_message_get_request_headers(message));
  if (declared >= 0 && static_cast<std::uint64_t>(declared) > max_bytes) {
    soup_message_body_set_accumulate(
        soup_server_message_get_request_body(message), FALSE);
    soup_server_message_set_status(
        message, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, nullptr);
    return nullptr;
  }

  if (uploads_in_flight.fetch_add(1) >= kMaxConcurrentUploads) {
    --uploads_in_flight;
    soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE,
                                   nullptr);
    return nullptr;
  }

  std::error_code error;
  std::filesystem::create_directories(staging_dir, error);
  if (error) {
    --uploads_in_flight;
    WEB_LOG(LogLevel::kWarning, "Could not create upload staging dir {}: {}",
            staging_dir.string(), error.message());
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return nullptr;
  }

  auto upload = std::make_unique<StagedUpload>();
  upload->max_bytes = max_bytes;
  upload->path = staging_dir / std::format(".upload-{}.part", upload_counter++);
  upload->file.open(upload->path, std::ios::binary | std::ios::trunc);
  if (!upload->file) {
    --uploads_in_flight;
    WEB_LOG(LogLevel::kWarning, "Could not stage upload at {}",
            upload->path.string());
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   nullptr);
    return nullptr;
  }

  // Chunks stream to the staged file; libsoup doesn't accumulate.
  soup_message_body_set_accumulate(
      soup_server_message_get_request_body(message), FALSE);
  g_signal_connect(message, "got-chunk", G_CALLBACK(&OnGotChunk), nullptr);

  auto* result = upload.get();
  g_object_set_data_full(G_OBJECT(message), kUploadKey, upload.release(),
                         &DeleteStagedUpload);
  return result;
}

StagedUpload* GetStagedUpload(SoupServerMessage* message) {
  return static_cast<StagedUpload*>(
      g_object_get_data(G_OBJECT(message), kUploadKey));
}

}  // namespace subtitler
