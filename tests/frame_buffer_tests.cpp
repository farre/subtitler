#include <doctest/doctest.h>
#include <gst/gst.h>

#include <optional>
#include <stop_token>

#include "stream/deleters.h"
#include "stream/frame_buffer.h"

namespace {

subtitler::BufferPtr MakeBuffer(std::uint64_t tag) {
  subtitler::BufferPtr buffer{gst_buffer_new_allocate(nullptr, 16, nullptr)};
  GST_BUFFER_PTS(buffer.get()) = tag;
  return buffer;
}

std::uint64_t Tag(const subtitler::BufferPtr& buffer) {
  return GST_BUFFER_PTS(buffer.get());
}

}  // namespace

TEST_CASE("frame buffer") {
  gst_init(nullptr, nullptr);

  SUBCASE("pops in FIFO order") {
    subtitler::FrameBuffer buffer{4};

    CHECK(buffer.PushLatest(MakeBuffer(1)));
    CHECK(buffer.PushLatest(MakeBuffer(2)));

    auto first = buffer.Pop(std::stop_token{});
    auto second = buffer.Pop(std::stop_token{});

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(Tag(*first) == 1);
    CHECK(Tag(*second) == 2);
  }

  SUBCASE("drops the oldest frame when full and counts it") {
    subtitler::FrameBuffer buffer{2};

    CHECK(buffer.PushLatest(MakeBuffer(1)));
    CHECK(buffer.PushLatest(MakeBuffer(2)));
    CHECK(buffer.PushLatest(MakeBuffer(3)));

    CHECK(buffer.DroppedFrames() == 1);

    auto frame = buffer.Pop(std::stop_token{});
    REQUIRE(frame.has_value());
    CHECK(Tag(*frame) == 2);
  }

  SUBCASE("flush discards queued frames without closing") {
    subtitler::FrameBuffer buffer{4};

    CHECK(buffer.PushLatest(MakeBuffer(1)));
    buffer.Flush();

    // The queue is empty, so Pop on an already-stopped token returns
    // nothing, and the buffer still accepts new frames.
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(buffer.Pop(stop.get_token()).has_value());

    CHECK(buffer.PushLatest(MakeBuffer(2)));
    auto frame = buffer.Pop(std::stop_token{});
    REQUIRE(frame.has_value());
    CHECK(Tag(*frame) == 2);
  }

  SUBCASE("flush bumps the generation") {
    subtitler::FrameBuffer buffer{4};

    const auto initial = buffer.Generation();
    CHECK(initial > 0);

    buffer.Flush();
    CHECK(buffer.Generation() != initial);

    const auto after_flush = buffer.Generation();
    buffer.Flush();
    CHECK(buffer.Generation() != after_flush);
  }

  SUBCASE("close rejects new frames and drains the queue") {
    subtitler::FrameBuffer buffer{4};

    CHECK(buffer.PushLatest(MakeBuffer(1)));
    buffer.Close();

    CHECK_FALSE(buffer.PushLatest(MakeBuffer(2)));
    CHECK(buffer.Pop(std::stop_token{}).has_value());
    CHECK_FALSE(buffer.Pop(std::stop_token{}).has_value());
  }
}
