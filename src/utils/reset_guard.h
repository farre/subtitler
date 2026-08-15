#pragma once

#include <tuple>
#include <utility>

namespace subtitler {

// Resets the pointers it refers to on destruction, in reverse order,
// unless released first.
template <typename... Ptrs>
class ResetGuard {
 public:
  explicit ResetGuard(Ptrs&... ptrs) noexcept : ptrs_(ptrs...) {}
  ~ResetGuard() { reset(std::make_index_sequence<sizeof...(Ptrs)>{}); }

  ResetGuard(const ResetGuard&) = delete;
  ResetGuard& operator=(const ResetGuard&) = delete;

  void release() noexcept { released_ = true; }

 private:
  template <std::size_t... Is>
  void reset(std::index_sequence<Is...>) {
    if (!released_) {
      (std::get<sizeof...(Is) - 1 - Is>(ptrs_).reset(), ...);
    }
  }

  std::tuple<Ptrs&...> ptrs_;
  bool released_ = false;
};

}  // namespace subtitler
