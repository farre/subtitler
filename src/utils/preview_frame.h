#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace subtitler {

struct PreviewFrame {
  std::uint64_t sequence;
  std::uint64_t pts_ns;
  std::shared_ptr<const std::vector<std::byte>> data;
};

// The shared latest-JPEG frame buffer between the preview pull thread in
// the stream module (producer) and the web module's HTTP handlers
// (consumers). Only the newest frame is kept; every Store wakes all
// waiters. Never touched from a GStreamer streaming thread.
class PreviewFrameBuffer {
 public:
  void Store(std::uint64_t pts_ns,
             std::shared_ptr<const std::vector<std::byte>> data) {
    {
      std::lock_guard lock{mutex_};
      frame_ = PreviewFrame{++sequence_, pts_ns, std::move(data)};
    }

    available_.notify_all();
  }

  std::optional<PreviewFrame> Latest() const {
    std::lock_guard lock{mutex_};
    return frame_;
  }

  // Blocks until a frame with sequence > last_seen is stored. Returns
  // nullopt when stop is requested first.
  std::optional<PreviewFrame> WaitNewer(std::uint64_t last_seen,
                                        std::stop_token stop) {
    std::unique_lock lock{mutex_};

    const auto newer = [this, last_seen] {
      return frame_.has_value() && frame_->sequence > last_seen;
    };

    if (!available_.wait(lock, stop, newer)) {
      return std::nullopt;
    }

    return frame_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable_any available_;
  std::optional<PreviewFrame> frame_;
  std::uint64_t sequence_ = 0;
};

}  // namespace subtitler
