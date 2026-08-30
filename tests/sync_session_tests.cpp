#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include "sync/sync_session.h"

namespace {

using State = subtitler::SyncSession::State;

constexpr std::int64_t kSecond = 1'000'000'000;

}  // namespace

TEST_CASE("sync session") {
  SUBCASE("locks once enough windows have fed and the matcher is stable") {
    int calls = 0;
    subtitler::SyncSession session{
        {},
        [&](const auto&, const auto&) {
          ++calls;
          return std::optional<std::int64_t>{2 * kSecond};
        },
        60 * kSecond};

    // Single windows never lock; the matcher isn't even consulted.
    CHECK(session.Feed({"a", 100}, kSecond).state == State::kListening);
    CHECK(session.Feed({"b", 200}, 2 * kSecond).state == State::kListening);
    CHECK(calls == 0);

    const auto result = session.Feed({"c", 300}, 3 * kSecond);
    REQUIRE(result.state == State::kSynced);
    // The position at lock time: running time plus the matched offset.
    CHECK(result.time_ms == 5000);
    CHECK(calls == 1);
  }

  SUBCASE("fails at the deadline without a lock") {
    subtitler::SyncSession session{
        {},
        [](const auto&, const auto&) { return std::optional<std::int64_t>{}; },
        45 * kSecond};

    CHECK(session.Feed({"a", 100}, kSecond).state == State::kListening);
    const auto result = session.Feed({"b", 200}, 46 * kSecond);
    REQUIRE(result.state == State::kFailed);
    CHECK_FALSE(result.reason.empty());
  }

  SUBCASE("Poll fails past the deadline without new input") {
    subtitler::SyncSession session{
        {},
        [](const auto&, const auto&) { return std::optional<std::int64_t>{}; },
        45 * kSecond};

    CHECK(session.Poll(10 * kSecond).state == State::kListening);
    CHECK(session.Poll(46 * kSecond).state == State::kFailed);
  }
}
