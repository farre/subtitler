#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "utils/preview_frame.h"

namespace {

std::shared_ptr<const std::vector<std::byte>> MakeData(std::byte fill) {
  return std::make_shared<const std::vector<std::byte>>(4, fill);
}

}  // namespace

TEST_CASE("PreviewFrameBuffer") {
  subtitler::PreviewFrameBuffer buffer;

  SUBCASE("Latest is empty before the first store") {
    CHECK_FALSE(buffer.Latest().has_value());
  }

  SUBCASE("Latest returns the stored frame") {
    buffer.Store(100, MakeData(std::byte{0xAB}));

    const auto frame = buffer.Latest();

    REQUIRE(frame.has_value());
    CHECK(frame->sequence == 1);
    CHECK(frame->pts_ns == 100);
    CHECK(frame->data->front() == std::byte{0xAB});
  }

  SUBCASE("Store replaces the previous frame and bumps the sequence") {
    buffer.Store(100, MakeData(std::byte{0x01}));
    buffer.Store(200, MakeData(std::byte{0x02}));

    const auto frame = buffer.Latest();

    REQUIRE(frame.has_value());
    CHECK(frame->sequence == 2);
    CHECK(frame->pts_ns == 200);
    CHECK(frame->data->front() == std::byte{0x02});
  }

  SUBCASE("WaitNewer returns an already-newer frame immediately") {
    buffer.Store(100, MakeData(std::byte{0x01}));

    const auto frame = buffer.WaitNewer(0, std::stop_token{});

    REQUIRE(frame.has_value());
    CHECK(frame->sequence == 1);
  }

  SUBCASE("WaitNewer blocks until a newer frame is stored") {
    std::atomic_bool waiting = true;

    std::jthread producer([&] {
      while (waiting.load()) {
        std::this_thread::yield();
      }
      buffer.Store(300, MakeData(std::byte{0x03}));
    });

    waiting.store(false);

    const auto frame = buffer.WaitNewer(0, std::stop_token{});

    REQUIRE(frame.has_value());
    CHECK(frame->sequence == 1);
    CHECK(frame->pts_ns == 300);
  }

  SUBCASE("WaitNewer ignores frames that are not newer") {
    buffer.Store(100, MakeData(std::byte{0x01}));

    std::jthread producer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
      buffer.Store(200, MakeData(std::byte{0x02}));
    });

    const auto frame = buffer.WaitNewer(1, std::stop_token{});

    REQUIRE(frame.has_value());
    CHECK(frame->sequence == 2);
  }

  SUBCASE("WaitNewer returns nullopt when already stopped") {
    std::stop_source source;
    source.request_stop();

    CHECK_FALSE(buffer.WaitNewer(0, source.get_token()).has_value());
  }

  SUBCASE("WaitNewer returns nullopt when stopped while waiting") {
    std::jthread stopper([](std::stop_token) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    });

    std::stop_source source;
    std::jthread requester([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
      source.request_stop();
    });

    CHECK_FALSE(buffer.WaitNewer(0, source.get_token()).has_value());
  }
}
