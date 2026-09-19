#pragma once

#include <cstdint>
#include <limits>

namespace subtitler {

// The largest absolute subtitle position/delay in milliseconds that the
// web API, the config, and the stream accept (#446). The stream keeps
// positions, delays, and anchors as signed nanoseconds and composes
// them (anchor = now - delay - position); bounding each millisecond
// input to a quarter of the int64 nanosecond range keeps both the
// ms→ns conversion and every composition overflow-free. ~73 years
// either way — negative values stay meaningful (a negative delay
// advances cues, a negative position seeks before the start).
inline constexpr std::int64_t kMaxSubtitleOffsetMs =
    (std::numeric_limits<std::int64_t>::max() / 4) / 1'000'000;

inline bool SubtitleOffsetMsValid(std::int64_t ms) {
  return ms >= -kMaxSubtitleOffsetMs && ms <= kMaxSubtitleOffsetMs;
}

// The cue font size range in points (#446): 1000 pt already dwarfs a
// 1080p frame.
inline constexpr std::int64_t kMaxSubtitleFontSizePt = 1000;

inline bool SubtitleFontSizeValid(std::int64_t size_pt) {
  return size_pt >= 1 && size_pt <= kMaxSubtitleFontSizePt;
}

}  // namespace subtitler
