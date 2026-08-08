#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

#include "stream/deleters.h"

namespace subtitler {
class FrameBuffer {
 public:
  explicit FrameBuffer(size_t capacity) : capacity_{capacity} {}

  bool push_latest(BufferPtr frame);

  std::optional<BufferPtr> pop(std::stop_token stop);

  void close();

  std::uint64_t dropped_frames() const noexcept;

 private:
  const std::size_t capacity_;

  mutable std::mutex mutex_;
  std::condition_variable_any available_;
  std::deque<BufferPtr> frames_;

  bool closed_ = false;
  std::atomic_uint64_t dropped_frames_ = 0;
};
}  // namespace subtitler
