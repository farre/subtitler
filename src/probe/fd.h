#pragma once

#include <unistd.h>

namespace subtitler::probe {

class Fd {
 public:
  explicit Fd(int fd = -1) noexcept : fd_{fd} {}
  ~Fd() { reset(); }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept : fd_{other.release()} {}
  Fd& operator=(Fd&& other) noexcept {
    reset(other.release());
    return *this;
  }

  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }

  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_;
};

}  // namespace subtitler::probe
