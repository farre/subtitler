#include "sync/sync_session.h"

#include <utility>

namespace subtitler {

namespace {

// The matcher is consulted once a few windows have collected; single
// windows never lock.
constexpr std::size_t kMinWindows = 3;

}  // namespace

SyncSession::SyncSession(std::vector<SrtCue> cues, SyncMatcher matcher,
                         std::int64_t deadline_ns)
    : cues_(std::move(cues)),
      matcher_(std::move(matcher)),
      deadline_ns_{deadline_ns} {}

SyncSession::Result SyncSession::Feed(TimestampedText window,
                                      std::int64_t now_ns) {
  windows_.push_back(std::move(window));

  if (windows_.size() >= kMinWindows) {
    if (const auto theta = matcher_(cues_, windows_)) {
      return {.state = State::kSynced,
              .time_ms = (now_ns + *theta) / 1'000'000,
              .reason = {}};
    }
  }

  return Poll(now_ns);
}

SyncSession::Result SyncSession::Poll(std::int64_t now_ns) const {
  if (now_ns > deadline_ns_) {
    return {.state = State::kFailed,
            .time_ms = std::nullopt,
            .reason = "no stable match within the listening window"};
  }
  return {};
}

}  // namespace subtitler
