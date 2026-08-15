#pragma once

#include <cstdint>
#include <string>

namespace subtitler::probe {

inline std::string FourccToString(std::uint32_t fourcc) {
  std::string result(4, '\0');
  for (int i = 0; i < 4; ++i) {
    result[i] = static_cast<char>((fourcc >> (8 * i)) & 0xff);
  }
  return result;
}

}  // namespace subtitler::probe
