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
    // The offset rides along so the applied position can advance with
    // the running time between the lock and its application.
    CHECK(result.theta_ns == 2 * kSecond);
    CHECK(session.WindowsFed() == 3);
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
    // An expired session consumed no evidence.
    CHECK(session.WindowsFed() == 1);
  }

  SUBCASE("a match after the deadline fails instead of locking") {
    int calls = 0;
    subtitler::SyncSession session{
        {},
        [&](const auto&, const auto&) {
          ++calls;
          return std::optional<std::int64_t>{2 * kSecond};
        },
        45 * kSecond};

    session.Feed({"a", 100}, kSecond);
    session.Feed({"b", 200}, 2 * kSecond);
    const auto result = session.Feed({"c", 300}, 46 * kSecond);
    REQUIRE(result.state == State::kFailed);
    // The matcher is never consulted with expired evidence.
    CHECK(calls == 0);
  }

  SUBCASE("the deadline is exclusive: equality is not expired") {
    subtitler::SyncSession session{
        {},
        [](const auto&, const auto&) {
          return std::optional<std::int64_t>{2 * kSecond};
        },
        45 * kSecond};

    session.Feed({"a", 100}, kSecond);
    session.Feed({"b", 200}, 2 * kSecond);
    const auto result = session.Feed({"c", 300}, 45 * kSecond);
    CHECK(result.state == State::kSynced);
  }

  SUBCASE("Poll fails past the deadline without new input") {
    subtitler::SyncSession session{
        {},
        [](const auto&, const auto&) { return std::optional<std::int64_t>{}; },
        45 * kSecond};

    CHECK(session.Poll(10 * kSecond).state == State::kListening);
    CHECK(session.Poll(45 * kSecond).state == State::kListening);
    CHECK(session.Poll(46 * kSecond).state == State::kFailed);
  }
}
