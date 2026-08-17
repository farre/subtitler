#include "stream/frame_buffer.h"

#include <utility>

namespace subtitler {
bool FrameBuffer::PushLatest(BufferPtr frame) {
  {
    std::lock_guard lock{mutex_};

    if (closed_) {
      return false;
    }

    if (frames_.size() == capacity_) {
      frames_.pop_front();
      ++dropped_frames_;
    }

    frames_.push_back(std::move(frame));
  }

  available_.notify_one();
  return true;
}

std::optional<BufferPtr> FrameBuffer::Pop(std::stop_token stop) {
  std::unique_lock lock{mutex_};

  available_.wait(lock, stop, [this] { return closed_ || !frames_.empty(); });

  if (frames_.empty()) {
    return std::nullopt;
  }

  auto frame = std::move(frames_.front());
  frames_.pop_front();

  return frame;
}

void FrameBuffer::Flush() {
  std::lock_guard lock{mutex_};
  frames_.clear();
}

void FrameBuffer::Close() {
  {
    std::lock_guard lock{mutex_};
    closed_ = true;
  }

  available_.notify_all();
}

std::uint64_t FrameBuffer::DroppedFrames() const noexcept {
  return dropped_frames_.load(std::memory_order_relaxed);
}
}  // namespace subtitler
