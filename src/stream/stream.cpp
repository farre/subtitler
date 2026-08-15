#include "stream/stream.h"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "stream/deleters.h"
#include "stream/frame_buffer.h"
#include "utils/reset_guard.h"

namespace {

using namespace subtitler;

constexpr int width = 1920;
constexpr int height = 1080;
constexpr int frames_per_second = 60;
constexpr auto frame_duration = GST_SECOND / frames_per_second;
constexpr std::uint64_t output_latency_frames = 3;
constexpr auto target_latency = output_latency_frames * frame_duration;
constexpr std::size_t frame_buffer_capacity = 4;

// No-signal screen: solid pink, BT.601 limited range.
constexpr std::uint8_t pink_y = 106;
constexpr std::uint8_t pink_u = 202;
constexpr std::uint8_t pink_v = 222;

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
  if (bus == nullptr) {
    return true;
  }

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

GstClockTime output_running_time(GstView<GstElement> output_pipeline,
                                 GstView<GstClock> output_clock) {
  const auto now = gst_clock_get_time(output_clock);
  const auto base = gst_element_get_base_time(output_pipeline);

  if (!GST_CLOCK_TIME_IS_VALID(base) || now < base) {
    return 0;
  }

  return now - base;
}

BufferPtr make_pink_frame() {
  BufferPtr buffer =
      BufferPtr{gst_buffer_new_allocate(nullptr, width * height * 2, nullptr)};

  if (buffer == nullptr) {
    return nullptr;
  }

  GstMapInfo info;

  if (!gst_buffer_map(buffer.get(), &info, GST_MAP_WRITE)) {
    return nullptr;
  }

  // YUY2 packs two pixels as Y U Y V.
  for (std::size_t i = 0; i + 4 <= info.size; i += 4) {
    info.data[i] = pink_y;
    info.data[i + 1] = pink_u;
    info.data[i + 2] = pink_y;
    info.data[i + 3] = pink_v;
  }

  gst_buffer_unmap(buffer.get(), &info);

  return buffer;
}

}  // namespace

namespace subtitler {

std::string capture_pipeline_description(std::string_view device) {
  return std::format(
      "v4l2src "
      "device=\"{}\" "
      "io-mode=mmap "
      "do-timestamp=true "
      "! video/x-raw,"
      "format=YUY2,"
      "width={},"
      "height={},"
      "framerate={}/1 "
      "! appsink "
      "name=capture_sink "
      "sync=false "
      "max-buffers=2 "
      "drop=true",
      device, width, height, frames_per_second);
}

std::string output_pipeline_description(OutputMode mode,
                                        std::optional<int> connector_id) {
  const auto connector = connector_id
                             ? std::format(" connector-id={}", *connector_id)
                             : std::string{};

  const std::string base = std::format(
      "appsrc "
      "name=output_source "
      "is-live=true "
      "format=time "
      "block=true "
      "max-buffers=2 ");

  switch (mode) {
    case OutputMode::kKmsPisp:
      return std::format(
          "{}"
          "! pispconvert "
          "name=converter "
          "output-buffer-count=4 "
          "! video/x-raw(memory:DMABuf),"
          "format=DMA_DRM,"
          "drm-format=NV12,"
          "width={},"
          "height={},"
          "framerate={}/1 "
          "! kmssink "
          "driver-name=vc4 "
          "force-modesetting=true "
          "sync=true"
          "{}",
          base, width, height, frames_per_second, connector);

    case OutputMode::kKmsSoftware:
      return std::format(
          "{}"
          "! videoconvert "
          "n-threads=4 "
          "! video/x-raw,"
          "format=NV16,"
          "width={},"
          "height={},"
          "framerate={}/1 "
          "! kmssink "
          "driver-name=vc4 "
          "force-modesetting=true "
          "sync=true"
          "{}",
          base, width, height, frames_per_second, connector);

    case OutputMode::kWindow:
      return std::format(
          "{}"
          "! videoconvert "
          "n-threads=4 "
          "! glimagesink sync=true",
          base);

    case OutputMode::kNull:
      return std::format("{}! fakesink sync=true", base);
  }

  return "";
}

struct Stream::Implementation {
  enum class CaptureState {
    kStopped,
    kCapturing,
    kScreensaver,
  };

  bool Initialize(const std::string& device, OutputMode output_mode,
                  std::optional<int> connector_id);

  ~Implementation() { Stop(); }

  bool StartCapture(const std::string& device);
  bool StartOutput(OutputMode output_mode, std::optional<int> connector_id);

  void Stop();

  void Poll();

  void RunScreensaver(std::stop_token stop);

  bool Failed() const {
    return capture_failed_.load() || output_failed_.load();
  }

  std::uint64_t dropped_frames() const { return frames_.dropped_frames(); }

  // Guards the pipelines, buses, threads, and capture_state_ below.
  std::mutex mutex_;

  FrameBuffer frames_{frame_buffer_capacity};

  std::atomic_bool capture_active_ = false;
  std::atomic_bool capture_failed_ = false;
  std::atomic_bool output_failed_ = false;

  CaptureState capture_state_ = CaptureState::kStopped;

  ElementPtr capture_pipeline_;
  ElementPtr capture_sink_;
  BusPtr capture_bus_;

  ElementPtr output_pipeline_;
  ElementPtr output_source_;
  BusPtr output_bus_;

  // Runs either the capture or the screensaver loop, never both.
  std::jthread capture_thread_;
  std::jthread output_thread_;
};

bool Stream::Implementation::StartCapture(const std::string& device) {
  std::lock_guard lock{mutex_};

  if (capture_thread_.joinable()) {
    capture_thread_.request_stop();

    // Changing state unblocks pending appsink operations.
    if (capture_pipeline_ != nullptr) {
      gst_element_set_state(capture_pipeline_.get(), GST_STATE_NULL);
    }

    capture_thread_.join();
    capture_bus_.reset();
    capture_sink_.reset();
    capture_pipeline_.reset();
  }

  capture_state_ = CaptureState::kStopped;

  ResetGuard reset{capture_pipeline_, capture_sink_, capture_bus_};

  capture_pipeline_ =
      parse_pipeline("capture", capture_pipeline_description(device));

  if (!capture_pipeline_) {
    std::println(stderr, "Couldn't create capture pipeline");
    return false;
  }

  capture_sink_ = ElementPtr{
      gst_bin_get_by_name(GST_BIN(capture_pipeline_.get()), "capture_sink")};

  if (!capture_sink_) {
    std::println(stderr, "Couldn't find appsink");
    return false;
  }

  capture_bus_ = BusPtr{gst_element_get_bus(capture_pipeline_.get())};

  if (gst_element_set_state(capture_pipeline_.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::println(stderr, "Could not start capture pipeline");
    return false;
  }

  // Live pipelines can return NO_PREROLL. That is not an error.
  gst_element_get_state(capture_pipeline_.get(), nullptr, nullptr,
                        2 * GST_SECOND);

  capture_active_.store(true);
  capture_failed_.store(false);

  capture_thread_ = std::jthread{[this](std::stop_token stop) {
    const auto sink = GstView<GstAppSink>{GST_APP_SINK(capture_sink_.get())};

    std::uint64_t fallback_frame_number = 0;

    while (!stop.stop_requested()) {
      SamplePtr sample =
          SamplePtr{gst_app_sink_try_pull_sample(sink, 100 * GST_MSECOND)};

      if (sample == nullptr) {
        if (gst_app_sink_is_eos(sink)) {
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

        capture_failed_.store(true);
        break;
      }

      if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(copied.get()))) {
        GST_BUFFER_PTS(copied.get()) = fallback_frame_number * frame_duration;
      }

      if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(copied.get()))) {
        GST_BUFFER_DURATION(copied.get()) = frame_duration;
      }

      ++fallback_frame_number;

      if (!frames_.push_latest(std::move(copied))) {
        break;
      }
    }

    capture_active_.store(false);
  }};

  capture_state_ = CaptureState::kCapturing;

  reset.release();
  return true;
}

bool Stream::Implementation::StartOutput(OutputMode output_mode,
                                         std::optional<int> connector_id) {
  std::lock_guard lock{mutex_};

  if (output_thread_.joinable()) {
    output_thread_.request_stop();

    // Changing state unblocks pending appsrc operations.
    if (output_pipeline_ != nullptr) {
      gst_element_set_state(output_pipeline_.get(), GST_STATE_NULL);
    }

    output_thread_.join();
    output_bus_.reset();
    output_source_.reset();
    output_pipeline_.reset();
  }

  ResetGuard reset{output_pipeline_, output_source_, output_bus_};

  output_pipeline_ = parse_pipeline(
      "output", output_pipeline_description(output_mode, connector_id));

  if (!output_pipeline_) {
    std::println(stderr, "Couldn't create output pipeline");
    return false;
  }

  output_source_ = ElementPtr{
      gst_bin_get_by_name(GST_BIN(output_pipeline_.get()), "output_source")};

  if (!output_source_) {
    std::println(stderr, "Couldn't find appsrc");
    return false;
  }

  {
    // The cast adds no reference; the view is non-owning and the
    // ElementPtr remains the sole owner.
    const auto source = GstView<GstAppSrc>{GST_APP_SRC(output_source_.get())};

    auto caps = CapsPtr{gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
        width, "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION,
        frames_per_second, 1, nullptr)};

    gst_app_src_set_caps(source, caps.get());
  }

  output_bus_ = BusPtr{gst_element_get_bus(output_pipeline_.get())};

  if (gst_element_set_state(output_pipeline_.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::println(stderr, "Could not start output pipeline");
    return false;
  }

  // Live pipelines can return NO_PREROLL. That is not an error.
  gst_element_get_state(output_pipeline_.get(), nullptr, nullptr,
                        2 * GST_SECOND);

  output_failed_.store(false);

  output_thread_ = std::jthread{[this](std::stop_token stop) {
    // The casts add no references; the views are non-owning and the
    // ElementPtrs remain the sole owners.
    const auto pipeline = GstView<GstElement>{output_pipeline_.get()};
    const auto source = GstView<GstAppSrc>{GST_APP_SRC(output_source_.get())};

    ClockPtr clock = ClockPtr{gst_element_get_clock(pipeline)};

    if (clock == nullptr) {
      std::println(stderr, "Output pipeline has no clock");

      output_failed_.store(true);
      return;
    }

    std::optional<GstClockTime> capture_anchor;
    std::optional<GstClockTime> output_anchor;
    GstClockTime previous_capture_pts = GST_CLOCK_TIME_NONE;

    while (!stop.stop_requested()) {
      auto frame_result = frames_.pop(stop);

      if (!frame_result) {
        break;
      }

      auto frame = std::move(*frame_result);

      const auto capture_pts = GST_BUFFER_PTS(frame.get());

      if (!GST_CLOCK_TIME_IS_VALID(capture_pts)) {
        std::println(stderr, "Buffered frame has no timestamp");

        output_failed_.store(true);
        break;
      }

      const bool timestamp_discontinuity =
          GST_CLOCK_TIME_IS_VALID(previous_capture_pts) &&
          capture_pts < previous_capture_pts;

      if (!capture_anchor || timestamp_discontinuity) {
        capture_anchor = capture_pts;

        output_anchor =
            output_running_time(pipeline, clock.get()) + target_latency;
      }

      const auto elapsed = capture_pts - *capture_anchor;

      GST_BUFFER_PTS(frame.get()) = *output_anchor + elapsed;

      GST_BUFFER_DTS(frame.get()) = GST_CLOCK_TIME_NONE;

      previous_capture_pts = capture_pts;

      // gst_app_src_push_buffer takes ownership.
      const auto result = gst_app_src_push_buffer(source, frame.release());

      if (result != GST_FLOW_OK) {
        if (!stop.stop_requested()) {
          std::println(stderr, "appsrc rejected frame: {}",
                       static_cast<int>(result));

          output_failed_.store(true);
        }

        break;
      }
    }

    gst_app_src_end_of_stream(source);
  }};

  reset.release();
  return true;
}

void Stream::Implementation::RunScreensaver(std::stop_token stop) {
  auto next_frame = std::chrono::steady_clock::now();
  std::uint64_t frame_number = 0;

  while (!stop.stop_requested()) {
    BufferPtr frame = make_pink_frame();

    if (frame == nullptr) {
      std::println(stderr, "Could not allocate no-signal frame");
      break;
    }

    GST_BUFFER_PTS(frame.get()) = frame_number * frame_duration;
    GST_BUFFER_DURATION(frame.get()) = frame_duration;

    ++frame_number;

    if (!frames_.push_latest(std::move(frame))) {
      break;
    }

    next_frame += std::chrono::nanoseconds{frame_duration};
    std::this_thread::sleep_until(next_frame);
  }
}

void Stream::Implementation::Stop() {
  std::lock_guard lock{mutex_};

  if (capture_thread_.joinable()) {
    capture_thread_.request_stop();
  }

  if (output_thread_.joinable()) {
    output_thread_.request_stop();
  }

  frames_.close();

  // Changing state unblocks pending appsink/appsrc operations.
  if (capture_pipeline_ != nullptr) {
    gst_element_set_state(capture_pipeline_.get(), GST_STATE_NULL);
  }

  if (output_pipeline_ != nullptr) {
    gst_element_set_state(output_pipeline_.get(), GST_STATE_NULL);
  }

  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }

  if (output_thread_.joinable()) {
    output_thread_.join();
  }

  capture_state_ = CaptureState::kStopped;
}

void Stream::Implementation::Poll() {
  std::lock_guard lock{mutex_};

  const bool capture_bus_ok = poll_bus(capture_bus_, "capture");
  poll_bus(output_bus_, "output");

  if (capture_state_ == CaptureState::kCapturing &&
      (!capture_bus_ok || !capture_active_.load())) {
    // Capture is gone; drop the pipeline and switch the capture thread
    // over to the no-signal screen.
    capture_thread_.request_stop();

    if (capture_pipeline_ != nullptr) {
      gst_element_set_state(capture_pipeline_.get(), GST_STATE_NULL);
    }

    capture_thread_.join();
    capture_bus_.reset();
    capture_sink_.reset();
    capture_pipeline_.reset();

    capture_state_ = CaptureState::kStopped;
  }

  if (capture_state_ == CaptureState::kStopped) {
    capture_state_ = CaptureState::kScreensaver;

    capture_thread_ =
        std::jthread{[this](std::stop_token stop) { RunScreensaver(stop); }};
  }
}

bool Stream::Implementation::Initialize(const std::string& device,
                                        OutputMode output_mode,
                                        std::optional<int> connector_id) {
  std::println("Capture pipeline:\n{}\n\nOutput pipeline:\n{}\n",
               capture_pipeline_description(device),
               output_pipeline_description(output_mode, connector_id));

  return StartOutput(output_mode, connector_id) && StartCapture(device);
}

Stream::~Stream() = default;

/* static */
std::unique_ptr<Stream> Stream::Create(const std::string& device,
                                       OutputMode output_mode,
                                       std::optional<int> connector_id) {
  static bool gst_initialized = false;
  if (!gst_initialized) {
    gst_initialized = true;
    gst_init(nullptr, nullptr);
  }

  auto implementation = std::make_unique<Implementation>();
  if (!implementation->Initialize(device, output_mode, connector_id)) {
    return nullptr;
  }

  auto stream = std::make_unique<Stream>();
  stream->implementation_ = std::move(implementation);
  return stream;
}

void Stream::Poll() { implementation_->Poll(); }

void Stream::Stop() { implementation_->Stop(); }

bool Stream::RestartCapture(const std::string& device) {
  return implementation_->StartCapture(device);
}

bool Stream::RestartOutput(OutputMode output_mode,
                           std::optional<int> connector_id) {
  return implementation_->StartOutput(output_mode, connector_id);
}

bool Stream::Failed() const { return implementation_->Failed(); }

std::uint64_t Stream::dropped_frames() const {
  return implementation_->dropped_frames();
}

}  // namespace subtitler
