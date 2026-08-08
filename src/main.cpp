#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/gstmessage.h>

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "stream/deleters.h"
#include "stream/description.h"

namespace {

constexpr int width = 1920;
constexpr int height = 1080;
constexpr int frames_per_second = 60;

// Application-owned queue capacity.
constexpr std::size_t frame_buffer_capacity = 4;

// Frames are timestamped this far ahead of the output clock.
constexpr std::uint64_t output_latency_frames = 3;

volatile std::sig_atomic_t signal_received = 0;

extern "C" void handle_signal(int) { signal_received = 1; }

template <typename T>
using GstPointer = std::unique_ptr<T, subtitler::GstDeleter<T>>;

using BufferPtr = GstPointer<GstBuffer>;
using ElementPtr = GstPointer<GstElement>;
using BusPtr = GstPointer<GstBus>;
using CapsPtr = GstPointer<GstCaps>;
using ClockPtr = GstPointer<GstClock>;
using MessagePtr = GstPointer<GstMessage>;
using SamplePtr = GstPointer<GstSample>;
using ErrorPtr = GstPointer<GError>;
using CharPtr = GstPointer<gchar>;

template <typename T>
using GstView = subtitler::GstView<T>;

class FrameBuffer {
 public:
  explicit FrameBuffer(std::size_t capacity) : capacity_{capacity} {}

  bool push_latest(BufferPtr frame) {
    {
      std::lock_guard lock{mutex_};

      if (closed_) {
        return false;
      }

      if (frames_.size() == capacity_) {
        frames_.pop_front();
        ++dropped_frames_;
      }

      frames_.push_back(std::move(frame));
    }

    available_.notify_one();
    return true;
  }

  std::optional<BufferPtr> pop(std::stop_token stop) {
    std::unique_lock lock{mutex_};

    available_.wait(lock, stop, [this] { return closed_ || !frames_.empty(); });

    if (frames_.empty()) {
      return std::nullopt;
    }

    auto frame = std::move(frames_.front());
    frames_.pop_front();

    return frame;
  }

  void close() {
    {
      std::lock_guard lock{mutex_};
      closed_ = true;
    }

    available_.notify_all();
  }

  std::uint64_t dropped_frames() const noexcept {
    return dropped_frames_.load(std::memory_order_relaxed);
  }

 private:
  const std::size_t capacity_;

  mutable std::mutex mutex_;
  std::condition_variable_any available_;
  std::deque<BufferPtr> frames_;

  bool closed_ = false;
  std::atomic_uint64_t dropped_frames_ = 0;
};

std::optional<int> parse_integer(std::string_view text) {
  int value{};

  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);

  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }

  return value;
}

ElementPtr parse_pipeline(std::string_view name,
                          const std::string& description) {
  ErrorPtr error;

  ElementPtr pipeline =
      ElementPtr{gst_parse_launch(description.c_str(), std::out_ptr(error))};

  if (error == nullptr && pipeline != nullptr) {
    return pipeline;
  }

  std::println(stderr, "Could not construct {} pipeline: {}", name,
               error != nullptr ? error->message : "unknown error");

  return nullptr;
}

void print_bus_error(std::string_view pipeline_name, MessagePtr& message) {
  ErrorPtr error;
  CharPtr debug;

  gst_message_parse_error(message.get(), std::out_ptr(error),
                          std::out_ptr(debug));

  std::println(stderr, "{} pipeline error from {}: {}", pipeline_name,
               GST_OBJECT_NAME(message->src),
               error != nullptr ? error->message : "unknown error");

  if (debug != nullptr) {
    std::println(stderr, "Debug information: {}", debug.get());
  }
}

bool poll_bus(BusPtr& bus, std::string_view pipeline_name) {
  auto message = MessagePtr{gst_bus_pop_filtered(
      bus.get(),
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))};

  if (message == nullptr) {
    return true;
  }

  const auto type = GST_MESSAGE_TYPE(message.get());

  if (type == GST_MESSAGE_ERROR) {
    print_bus_error(pipeline_name, message);
  } else {
    std::println("{} pipeline reached EOS", pipeline_name);
  }

  return false;
}

GstClockTime output_running_time(ElementPtr& output_pipeline,
                                 ClockPtr& output_clock) {
  const auto now = gst_clock_get_time(output_clock.get());
  const auto base = gst_element_get_base_time(output_pipeline.get());

  if (!GST_CLOCK_TIME_IS_VALID(base) || now < base) {
    return 0;
  }

  return now - base;
}

}  // namespace

int main(int argc, char** argv) {
  std::string device = "/dev/video0";
  subtitler::OutputMode output_mode = subtitler::OutputMode::kKmsSoftware;
  std::optional<int> connector_id;
  int positional = 0;

  const auto usage = [&] {
    std::println(stderr,
                 "Usage: {} [video-device] [connector-id] "
                 "[--output=software|pisp|window|null]",
                 argv[0]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};

    if (arg == "--output=pisp") {
      output_mode = subtitler::OutputMode::kKmsPisp;
    } else if (arg == "--output=software") {
      output_mode = subtitler::OutputMode::kKmsSoftware;
    } else if (arg == "--output=window") {
      output_mode = subtitler::OutputMode::kWindow;
    } else if (arg == "--output=null") {
      output_mode = subtitler::OutputMode::kNull;
    } else if (arg.starts_with("--")) {
      std::println(stderr, "Unknown option: {}", arg);
      usage();
      return EXIT_FAILURE;
    } else if (positional == 0) {
      device = arg;
      ++positional;
    } else if (positional == 1) {
      connector_id = parse_integer(arg);

      if (!connector_id) {
        std::println(stderr, "Invalid DRM connector ID: {}", arg);
        return EXIT_FAILURE;
      }

      ++positional;
    } else {
      usage();
      return EXIT_FAILURE;
    }
  }

  gst_init(nullptr, nullptr);

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  const auto capture_description =
      subtitler::capture_pipeline_description(device);

  const auto output_description =
      subtitler::output_pipeline_description(output_mode, connector_id);

  std::println("Capture pipeline:\n{}\n\nOutput pipeline:\n{}\n",
               capture_description, output_description);

  auto capture_pipeline = parse_pipeline("capture", capture_description);

  auto output_pipeline = parse_pipeline("output", output_description);

  if (capture_pipeline == nullptr || output_pipeline == nullptr) {
    return EXIT_FAILURE;
  }

  auto capture_sink_element = ElementPtr{
      gst_bin_get_by_name(GST_BIN(capture_pipeline.get()), "capture_sink")};

  auto output_source_element = ElementPtr{
      gst_bin_get_by_name(GST_BIN(output_pipeline.get()), "output_source")};

  if (capture_sink_element == nullptr || output_source_element == nullptr) {
    std::println(stderr, "Could not find appsink or appsrc");

    return EXIT_FAILURE;
  }

  // The casts add no reference; the views are non-owning and the
  // ElementPtrs above remain the sole owners.
  const auto capture_sink =
      GstView<GstAppSink>{GST_APP_SINK(capture_sink_element.get())};
  const auto output_source =
      GstView<GstAppSrc>{GST_APP_SRC(output_source_element.get())};

  {
    auto output_caps = CapsPtr{gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
        width, "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION,
        frames_per_second, 1, nullptr)};

    gst_app_src_set_caps(output_source, output_caps.get());
  }

  auto capture_bus = BusPtr{gst_element_get_bus(capture_pipeline.get())};
  auto output_bus = BusPtr{gst_element_get_bus(output_pipeline.get())};

  if (gst_element_set_state(output_pipeline.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::println(stderr, "Could not start output pipeline");
    return EXIT_FAILURE;
  }

  if (gst_element_set_state(capture_pipeline.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::println(stderr, "Could not start capture pipeline");
    return EXIT_FAILURE;
  }

  // Live pipelines can return NO_PREROLL. That is not an error.
  gst_element_get_state(output_pipeline.get(), nullptr, nullptr,
                        2 * GST_SECOND);

  gst_element_get_state(capture_pipeline.get(), nullptr, nullptr,
                        2 * GST_SECOND);

  FrameBuffer frames{frame_buffer_capacity};
  std::atomic_bool worker_failed = false;

  constexpr auto frame_duration = GST_SECOND / frames_per_second;

  constexpr auto target_latency = output_latency_frames * frame_duration;

  std::jthread capture_thread{[&](std::stop_token stop) {
    std::uint64_t fallback_frame_number = 0;

    while (!stop.stop_requested()) {
      SamplePtr sample = SamplePtr{
          gst_app_sink_try_pull_sample(capture_sink, 100 * GST_MSECOND)};

      if (sample == nullptr) {
        if (gst_app_sink_is_eos(capture_sink)) {
          break;
        }

        continue;
      }

      auto copy_sample = [](SamplePtr& sample) -> BufferPtr {
        // gst_sample_get_buffer is transfer-none.
        const GstView<GstBuffer> captured = gst_sample_get_buffer(sample.get());
        if (captured == nullptr) {
          std::println(stderr, "Captured sample contained no buffer");

          return nullptr;
        }

        return BufferPtr{gst_buffer_copy_deep(captured)};
      };

      // This creates application-owned memory rather than
      // retaining a reference to a V4L2 driver buffer.
      BufferPtr copied = copy_sample(sample);

      if (copied == nullptr) {
        std::println(stderr, "Could not copy captured frame");

        worker_failed.store(true);
        break;
      }

      if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(copied.get()))) {
        GST_BUFFER_PTS(copied.get()) = fallback_frame_number * frame_duration;
      }

      if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(copied.get()))) {
        GST_BUFFER_DURATION(copied.get()) = frame_duration;
      }

      ++fallback_frame_number;

      if (!frames.push_latest(std::move(copied))) {
        break;
      }
    }

    frames.close();
  }};

  std::jthread output_thread{[&](std::stop_token stop) {
    ClockPtr clock = ClockPtr{gst_element_get_clock(output_pipeline.get())};

    if (clock == nullptr) {
      std::println(stderr, "Output pipeline has no clock");

      worker_failed.store(true);
      return;
    }

    std::optional<GstClockTime> capture_anchor;
    std::optional<GstClockTime> output_anchor;
    GstClockTime previous_capture_pts = GST_CLOCK_TIME_NONE;

    while (!stop.stop_requested()) {
      auto frame_result = frames.pop(stop);

      if (!frame_result) {
        break;
      }

      auto frame = std::move(*frame_result);

      const auto capture_pts = GST_BUFFER_PTS(frame.get());

      if (!GST_CLOCK_TIME_IS_VALID(capture_pts)) {
        std::println(stderr, "Buffered frame has no timestamp");

        worker_failed.store(true);
        break;
      }

      const bool timestamp_discontinuity =
          GST_CLOCK_TIME_IS_VALID(previous_capture_pts) &&
          capture_pts < previous_capture_pts;

      if (!capture_anchor || timestamp_discontinuity) {
        capture_anchor = capture_pts;

        output_anchor =
            output_running_time(output_pipeline, clock) + target_latency;
      }

      const auto elapsed = capture_pts - *capture_anchor;

      GST_BUFFER_PTS(frame.get()) = *output_anchor + elapsed;

      GST_BUFFER_DTS(frame.get()) = GST_CLOCK_TIME_NONE;

      previous_capture_pts = capture_pts;

      // gst_app_src_push_buffer takes ownership.
      const auto result =
          gst_app_src_push_buffer(output_source, frame.release());

      if (result != GST_FLOW_OK) {
        if (!stop.stop_requested()) {
          std::println(stderr, "appsrc rejected frame: {}",
                       static_cast<int>(result));

          worker_failed.store(true);
        }

        break;
      }
    }

    gst_app_src_end_of_stream(output_source);
  }};

  bool running = true;

  while (running && !signal_received) {
    if (worker_failed.load()) {
      running = false;
      break;
    }

    running =
        poll_bus(capture_bus, "capture") && poll_bus(output_bus, "output");

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  capture_thread.request_stop();
  output_thread.request_stop();
  frames.close();

  // Changing state unblocks pending appsink/appsrc operations.
  gst_element_set_state(capture_pipeline.get(), GST_STATE_NULL);

  gst_element_set_state(output_pipeline.get(), GST_STATE_NULL);

  capture_thread.join();
  output_thread.join();

  std::println("Stopped. Application buffer dropped {} frames.",
               frames.dropped_frames());

  return worker_failed.load() ? EXIT_FAILURE : EXIT_SUCCESS;
}
