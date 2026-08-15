#pragma once

#include <memory>

namespace subtitler {

// Deleter for C APIs that release an object through a free function, e.g.
// UniquePtr<drmModeRes, drmModeFreeResources>.
template <auto Free>
struct FunctionDeleter {
  template <typename T>
  void operator()(T* object) const noexcept {
    if (object != nullptr) {
      Free(object);
    }
  }
};

template <typename T, auto Free>
using UniquePtr = std::unique_ptr<T, FunctionDeleter<Free>>;

}  // namespace subtitler
