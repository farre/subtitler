#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

#include "stream/deleters.h"

namespace subtitler {
class FrameBuffer {
 public:
  explicit FrameBuffer(size_t capacity) : capacity_{capacity} {}

  bool PushLatest(BufferPtr frame);

  std::optional<BufferPtr> Pop(std::stop_token stop);

  // Discards any queued frames without closing the buffer. Bumps the
  // generation so consumers can mark what comes next as discontinuous.
  void Flush();

  void Close();

  // Incremented by every Flush; starts at 1.
  std::uint64_t Generation() const noexcept;

  std::uint64_t DroppedFrames() const noexcept;

 private:
  const std::size_t capacity_;

  mutable std::mutex mutex_;
  std::condition_variable_any available_;
  std::deque<BufferPtr> frames_;

  bool closed_ = false;
  std::atomic_uint64_t dropped_frames_ = 0;
  std::atomic_uint64_t generation_ = 1;
};
}  // namespace subtitler
