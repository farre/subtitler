#include "stream/stream.h"

#include <alsa/asoundlib.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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
#include <vector>

#include "stream/deleters.h"
#include "stream/frame_buffer.h"
#include "stream/output_anchor.h"
#include "stream/preview_gate.h"
#include "utils/reset_guard.h"

namespace {

using namespace subtitler;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFramesPerSecond = 60;
constexpr auto kFrameDuration = GST_SECOND / kFramesPerSecond;
constexpr std::uint64_t kOutputLatencyFrames = 3;
constexpr auto kTargetLatency = kOutputLatencyFrames * kFrameDuration;
constexpr std::size_t kFrameBufferCapacity = 4;

// CV105 audio: S16_LE 48 kHz stereo only (docs/pi-setup.md). Bit-transparent
// passthrough: no element may touch the samples.
constexpr std::string_view kAudioDevice = "hw:CARD=Video,DEV=0";
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr std::uint64_t kAudioBytesPerFrame = kAudioChannels * 2;
constexpr std::size_t kAudioBufferCapacity = 8;
// One silence chunk per video frame period.
constexpr std::uint64_t kSilenceFramesPerChunk = kAudioRate / kFramesPerSecond;

// No-signal screen: solid pink, BT.601 limited range.
constexpr std::uint8_t kPinkY = 106;
constexpr std::uint8_t kPinkU = 202;
constexpr std::uint8_t kPinkV = 222;

// MJPEG web preview (#376): one shared encoder branch, teed off the appsrc
// output — the point where the subtitle overlay will sit — so the preview
// shows composited video in a format jpegenc accepts (NV16/SAND DMABuf are
// unreachable for it).
constexpr int kPreviewWidth = 640;
constexpr int kPreviewHeight = 360;
constexpr int kPreviewFramesPerSecond = 10;
constexpr int kPreviewJpegQuality = 75;

// vc4-hdmi playback accepts only IEC958 subframes, so address it through
// alsa-lib's plug layer. Opening a port with no display attached fails with
// ENOTSUPP, so the first port that opens is the connected one.
std::string DefaultAudioOutputDevice() {
  for (int port = 0; port < 2; ++port) {
    const auto probe = std::format("hw:CARD=vc4hdmi{},DEV=0", port);
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, probe.c_str(), SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK) == 0) {
      snd_pcm_close(pcm);
      return std::format("plughw:CARD=vc4hdmi{},DEV=0", port);
    }
  }

  return "plughw:CARD=vc4hdmi0,DEV=0";
}

ElementPtr ParsePipeline(std::string_view name,
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

void PrintBusError(std::string_view pipeline_name, MessagePtr& message) {
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

bool PollBus(BusPtr& bus, std::string_view pipeline_name) {
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
    PrintBusError(pipeline_name, message);
  } else {
    std::println("{} pipeline reached EOS", pipeline_name);
  }

  return false;
}

GstClockTime OutputRunningTime(GstView<GstElement> output_pipeline,
                               GstView<GstClock> output_clock) {
  const auto now = gst_clock_get_time(output_clock);
  const auto base = gst_element_get_base_time(output_pipeline);

  if (!GST_CLOCK_TIME_IS_VALID(base) || now < base) {
    return 0;
  }

  return now - base;
}
BufferPtr MakeSolidFrame(int width, int height) {
  BufferPtr buffer = BufferPtr{
      gst_buffer_new_allocate(nullptr, width * height * 2, nullptr)};

  if (buffer == nullptr) {
    return nullptr;
  }

  GstMapInfo info;

  if (!gst_buffer_map(buffer.get(), &info, GST_MAP_WRITE)) {
    return nullptr;
  }

  // YUY2 packs two pixels as Y U Y V.
  for (std::size_t i = 0; i + 4 <= info.size; i += 4) {
    info.data[i] = kPinkY;
    info.data[i + 1] = kPinkU;
    info.data[i + 2] = kPinkY;
    info.data[i + 3] = kPinkV;
  }

  gst_buffer_unmap(buffer.get(), &info);

  return buffer;
}

BufferPtr MakePinkFrame() { return MakeSolidFrame(kWidth, kHeight); }

BufferPtr MakeSilence() {
  BufferPtr buffer = BufferPtr{gst_buffer_new_allocate(
      nullptr, kSilenceFramesPerChunk * kAudioBytesPerFrame, nullptr)};

  if (buffer == nullptr) {
    return nullptr;
  }

  // S16LE digital silence is all zeroes.
  gst_buffer_memset(buffer.get(), 0, 0,
                    kSilenceFramesPerChunk * kAudioBytesPerFrame);

  return buffer;
}

// One-shot encode of the magenta placeholder frame that seeds the preview
// frame store, so the web endpoints always have a frame to serve —
// symmetric with the no-signal screen. (The kPink constants are full
// magenta in BT.601 limited range, so placeholder and no-signal screen
// match.) Real frames replace it once the preview gate opens.
std::shared_ptr<const std::vector<std::byte>> EncodePlaceholderJpeg() {
  const std::string description = std::format(
      "appsrc name=seed_source ! jpegenc quality={} ! appsink name=seed_sink",
      kPreviewJpegQuality);

  ErrorPtr error;
  ElementPtr pipeline{gst_parse_launch(description.c_str(), std::out_ptr(error))};

  if (error != nullptr || pipeline == nullptr) {
    return nullptr;
  }

  ElementPtr source{
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "seed_source")};
  ElementPtr sink{gst_bin_get_by_name(GST_BIN(pipeline.get()), "seed_sink")};

  if (!source || !sink) {
    return nullptr;
  }

  {
    const auto src = GstView<GstAppSrc>{GST_APP_SRC(source.get())};
    auto caps = CapsPtr{gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
        kPreviewWidth, "height", G_TYPE_INT, kPreviewHeight, "framerate",
        GST_TYPE_FRACTION, kPreviewFramesPerSecond, 1, nullptr)};
    gst_app_src_set_caps(src, caps.get());
  }

  if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    return nullptr;
  }

  BufferPtr frame = MakeSolidFrame(kPreviewWidth, kPreviewHeight);

  if (frame == nullptr) {
    return nullptr;
  }

  GST_BUFFER_PTS(frame.get()) = 0;
  GST_BUFFER_DURATION(frame.get()) = GST_SECOND / kPreviewFramesPerSecond;

  {
    const auto src = GstView<GstAppSrc>{GST_APP_SRC(source.get())};

    if (gst_app_src_push_buffer(src, frame.release()) != GST_FLOW_OK) {
      return nullptr;
    }

    gst_app_src_end_of_stream(src);
  }

  const auto app_sink = GstView<GstAppSink>{GST_APP_SINK(sink.get())};

  SamplePtr sample;
  for (int attempt = 0; attempt < 20 && sample == nullptr; ++attempt) {
    sample = SamplePtr{gst_app_sink_try_pull_sample(app_sink, 100 * GST_MSECOND)};
  }

  gst_element_set_state(pipeline.get(), GST_STATE_NULL);

  if (sample == nullptr) {
    return nullptr;
  }

  // gst_sample_get_buffer is transfer-none.
  const GstView<GstBuffer> encoded = gst_sample_get_buffer(sample.get());

  GstMapInfo info;

  if (encoded == nullptr || !gst_buffer_map(encoded, &info, GST_MAP_READ)) {
    return nullptr;
  }

  const auto* begin = reinterpret_cast<const std::byte*>(info.data);
  auto data = std::make_shared<const std::vector<std::byte>>(
      begin, begin + info.size);

  gst_buffer_unmap(encoded, &info);

  return data;
}

BufferPtr CopyCapturedBuffer(GstView<GstSample> sample) {
  // gst_sample_get_buffer is transfer-none.
  const GstView<GstBuffer> captured = gst_sample_get_buffer(sample);

  if (captured == nullptr) {
    std::println(stderr, "Captured sample contained no buffer");

    return nullptr;
  }

  return BufferPtr{gst_buffer_copy_deep(captured)};
}

}  // namespace

namespace subtitler {

std::string CapturePipelineDescription(std::string_view device, bool audio) {
  auto description = VideoCapturePipelineDescription(device);
  if (audio) {
    description += " " + AudioCapturePipelineDescription(kAudioDevice);
  }
  return description;
}

std::string VideoCapturePipelineDescription(std::string_view device) {
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
      device, kWidth, kHeight, kFramesPerSecond);
}

std::string AudioCapturePipelineDescription(std::string_view device) {
  // A single linear chain so that a tee for the whisper tap (#19) can be
  // inserted without touching the passthrough path.
  return std::format(
      "alsasrc "
      "device=\"{}\" "
      "do-timestamp=true "
      "! audio/x-raw,"
      "format=S16LE,"
      "rate={},"
      "channels={} "
      "! appsink "
      "name=capture_audio_sink "
      "sync=false "
      "max-buffers=8 "
      "drop=true",
      device, kAudioRate, kAudioChannels);
}

std::string OutputPipelineDescription(
    OutputMode mode, std::optional<int> connector_id,
    std::optional<std::string_view> audio_device, bool preview) {
  auto description = VideoOutputPipelineDescription(mode, connector_id, preview);
  if (audio_device) {
    description += " " + AudioOutputPipelineDescription(*audio_device);
  }
  return description;
}

// The preview side of the output tee. The leaky queue is the only coupling
// to the HDMI branch and never blocks it; the gate installed on the queue
// (InstallPreviewGate) drops all branch buffers until a web client
// activates the preview (#384). async=false is load-bearing: with the gate
// closed the appsink starves, and a starving sink's preroll would
// otherwise block the whole pipeline (HDMI included) from reaching
// PLAYING.
std::string PreviewOutputPipelineDescription() {
  return std::format(
      "output_tee. "
      "! queue "
      "name=preview_queue "
      "max-size-buffers=1 "
      "max-size-bytes=0 "
      "max-size-time=0 "
      "leaky=downstream "
      "! videoscale "
      "! video/x-raw,"
      "width={},"
      "height={} "
      "! videorate "
      "! video/x-raw,"
      "framerate={}/1 "
      "! jpegenc "
      "quality={} "
      "! appsink "
      "name=preview_sink "
      "sync=false "
      "async=false "
      "max-buffers=1 "
      "drop=true",
      kPreviewWidth, kPreviewHeight, kPreviewFramesPerSecond,
      kPreviewJpegQuality);
}

std::string AudioOutputPipelineDescription(std::string_view device) {
  return std::format(
      "appsrc "
      "name=output_audio_source "
      "is-live=true "
      "format=time "
      "block=true "
      "max-buffers=8 "
      "! alsasink "
      "device=\"{}\" "
      "slave-method=skew",
      device);
}

std::string VideoOutputPipelineDescription(OutputMode mode,
                                           std::optional<int> connector_id,
                                           bool preview) {
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

  std::string tail;

  switch (mode) {
    case OutputMode::kKmsPisp:
      tail = std::format(
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
          kWidth, kHeight, kFramesPerSecond, connector);
      break;

    case OutputMode::kKmsSoftware:
      tail = std::format(
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
          kWidth, kHeight, kFramesPerSecond, connector);
      break;

    case OutputMode::kWindow:
      tail = std::format(
          "! videoconvert "
          "n-threads=4 "
          "! glimagesink sync=true");
      break;

    case OutputMode::kNull:
      tail = "! fakesink sync=true";
      break;
  }

  if (!preview) {
    return base + tail;
  }

  // The tee sits right after the appsrc, where the subtitle overlay will
  // be inserted, so the preview taps composited video (#379). Every tee
  // branch needs its own queue: without one the clock-synced sink blocks
  // the tee's streaming thread in preroll and the pipeline deadlocks.
  // Sized 1 and not leaky so HDMI frames are never dropped here.
  return std::format(
      "{}! tee name=output_tee output_tee. "
      "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 {} {}",
      base, tail, PreviewOutputPipelineDescription());
}

struct Stream::Implementation {
  enum class CaptureState {
    kStopped,
    kCapturing,
    kScreensaver,
  };

  Implementation(std::size_t frame_capacity, std::size_t audio_capacity)
      : frames_(frame_capacity), audio_(audio_capacity) {}

  bool Initialize(const std::string& device, OutputMode output_mode,
                  std::optional<int> connector_id, bool audio,
                  const std::optional<std::string>& audio_output_device,
                  std::int64_t audio_offset_ms, bool preview);

  ~Implementation() { Stop(); }

  bool StartCapture(const std::string& device);
  bool StartOutput(OutputMode output_mode, std::optional<int> connector_id);

  void Stop();

  void Poll();

  void RunCapture(std::stop_token stop);
  void RunAudioCapture(std::stop_token stop);
  void RunOutput(std::stop_token stop);
  void RunAudioOutput(std::stop_token stop);
  void RunScreensaver(std::stop_token stop);
  void RunPreview(std::stop_token stop);

  void SetPreviewActive(bool active);

  PreviewFrameBuffer& PreviewFrames() { return preview_frames_; }

  // Tears down one pipeline and its thread. The lock argument proves the
  // caller holds mutex_.
  void StopCapturePipeline(const std::lock_guard<std::mutex>&);
  void StopOutputPipeline(const std::lock_guard<std::mutex>&);

  bool Failed() const {
    return capture_failed_.load() || output_failed_.load();
  }

  std::uint64_t DroppedFrames() const { return frames_.DroppedFrames(); }

  // Guards the pipelines, buses, threads, and capture_state_ below.
  std::mutex mutex_;

  FrameBuffer frames_;
  FrameBuffer audio_;

  std::atomic_bool capture_active_ = false;
  std::atomic_bool capture_failed_ = false;
  std::atomic_bool output_failed_ = false;

  bool audio_enabled_ = false;
  std::string audio_output_device_;
  // Signed nanoseconds added to audio timestamps at output re-anchoring.
  std::int64_t audio_offset_ = 0;

  // Set at Initialize: whether the output pipeline has a preview branch.
  bool preview_enabled_ = false;
  // Read by the preview gate's pad probe on the streaming thread.
  std::atomic_bool preview_active_ = false;
  PreviewFrameBuffer preview_frames_;

  CaptureState capture_state_ = CaptureState::kStopped;

  ElementPtr capture_pipeline_;
  ElementPtr capture_sink_;
  ElementPtr capture_audio_sink_;
  BusPtr capture_bus_;

  ElementPtr output_pipeline_;
  ElementPtr output_source_;
  ElementPtr output_audio_source_;
  ElementPtr preview_sink_;
  ElementPtr preview_queue_;
  BusPtr output_bus_;

  // Runs either the capture or the screensaver loop, never both.
  std::jthread capture_thread_;
  // Drains the audio branch while capturing; joinable only when audio is
  // enabled.
  std::jthread audio_thread_;
  std::jthread output_thread_;
  // Feeds the audio branch of the output pipeline; joinable only when
  // audio is enabled.
  std::jthread audio_output_thread_;
  // Drains the preview appsink into preview_frames_; joinable only when
  // the preview branch is enabled.
  std::jthread preview_thread_;
};

bool Stream::Implementation::StartCapture(const std::string& device) {
  std::lock_guard lock{mutex_};

  StopCapturePipeline(lock);

  ResetGuard reset{capture_pipeline_, capture_sink_, capture_audio_sink_,
                   capture_bus_};

  capture_pipeline_ = ParsePipeline(
      "capture", CapturePipelineDescription(device, audio_enabled_));

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

  if (audio_enabled_) {
    capture_audio_sink_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(capture_pipeline_.get()), "capture_audio_sink")};

    if (!capture_audio_sink_) {
      std::println(stderr, "Couldn't find audio appsink");
      return false;
    }
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

  capture_thread_ =
      std::jthread{[this](std::stop_token stop) { RunCapture(stop); }};

  if (audio_enabled_) {
    audio_thread_ =
        std::jthread{[this](std::stop_token stop) { RunAudioCapture(stop); }};
  }

  capture_state_ = CaptureState::kCapturing;

  reset.release();
  return true;
}

void Stream::Implementation::RunCapture(std::stop_token stop) {
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

    // This creates application-owned memory rather than
    // retaining a reference to a V4L2 driver buffer.
    BufferPtr copied = CopyCapturedBuffer(sample.get());

    if (copied == nullptr) {
      std::println(stderr, "Could not copy captured frame");

      capture_failed_.store(true);
      break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(copied.get()))) {
      GST_BUFFER_PTS(copied.get()) = fallback_frame_number * kFrameDuration;
    }

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(copied.get()))) {
      GST_BUFFER_DURATION(copied.get()) = kFrameDuration;
    }

    ++fallback_frame_number;

    if (!frames_.PushLatest(std::move(copied))) {
      break;
    }
  }

  capture_active_.store(false);
}

void Stream::Implementation::RunAudioCapture(std::stop_token stop) {
  const auto sink =
      GstView<GstAppSink>{GST_APP_SINK(capture_audio_sink_.get())};

  std::uint64_t fallback_sample_number = 0;

  while (!stop.stop_requested()) {
    SamplePtr sample =
        SamplePtr{gst_app_sink_try_pull_sample(sink, 100 * GST_MSECOND)};

    if (sample == nullptr) {
      if (gst_app_sink_is_eos(sink)) {
        break;
      }

      continue;
    }

    BufferPtr copied = CopyCapturedBuffer(sample.get());

    if (copied == nullptr) {
      std::println(stderr, "Could not copy captured audio");

      capture_failed_.store(true);
      break;
    }

    const auto frames = gst_buffer_get_size(copied.get()) / kAudioBytesPerFrame;

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(copied.get()))) {
      GST_BUFFER_PTS(copied.get()) =
          fallback_sample_number * GST_SECOND / kAudioRate;
    }

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(copied.get()))) {
      GST_BUFFER_DURATION(copied.get()) = frames * GST_SECOND / kAudioRate;
    }

    fallback_sample_number += frames;

    if (!audio_.PushLatest(std::move(copied))) {
      break;
    }
  }

  capture_active_.store(false);
}

bool Stream::Implementation::StartOutput(OutputMode output_mode,
                                         std::optional<int> connector_id) {
  std::lock_guard lock{mutex_};

  StopOutputPipeline(lock);

  ResetGuard reset{output_pipeline_, output_source_, output_audio_source_,
                   preview_sink_, preview_queue_, output_bus_};

  const std::optional<std::string_view> audio_device =
      audio_enabled_
          ? std::make_optional<std::string_view>(audio_output_device_)
          : std::nullopt;

  output_pipeline_ = ParsePipeline(
      "output", OutputPipelineDescription(output_mode, connector_id,
                                          audio_device, preview_enabled_));

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
        kWidth, "height", G_TYPE_INT, kHeight, "framerate", GST_TYPE_FRACTION,
        kFramesPerSecond, 1, nullptr)};

    gst_app_src_set_caps(source, caps.get());
  }

  if (audio_enabled_) {
    output_audio_source_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "output_audio_source")};

    if (!output_audio_source_) {
      std::println(stderr, "Couldn't find audio appsrc");
      return false;
    }

    const auto source =
        GstView<GstAppSrc>{GST_APP_SRC(output_audio_source_.get())};

    auto caps = CapsPtr{gst_caps_new_simple(
        "audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT,
        kAudioRate, "channels", G_TYPE_INT, kAudioChannels, "layout",
        G_TYPE_STRING, "interleaved", nullptr)};

    gst_app_src_set_caps(source, caps.get());
  }

  if (preview_enabled_) {
    preview_sink_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "preview_sink")};
    preview_queue_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "preview_queue")};

    if (!preview_sink_ || !preview_queue_) {
      std::println(stderr, "Couldn't find preview branch elements");
      return false;
    }

    // The gate drops all preview branch buffers while there are no web
    // clients, so no JPEG encoding happens without watchers (#384).
    InstallPreviewGate(preview_queue_.get(), preview_active_);
  }

  output_bus_ = BusPtr{gst_element_get_bus(output_pipeline_.get())};
  {
    // Pin the pipeline clock to the system clock. Otherwise adding the
    // alsasink makes the pipeline adopt the audio ring-buffer clock,
    // which starts at zero when the device starts and breaks the anchor
    // math. The alsasink skews its device clock onto the pipeline clock
    // (slave-method=skew) regardless.
    ClockPtr clock{gst_system_clock_obtain()};
    gst_pipeline_use_clock(GST_PIPELINE(output_pipeline_.get()), clock.get());

    // Pin the pipeline latency to the target latency. Sinks render at
    // PTS + configured latency; an alsasink's reported latency (hundreds
    // of ms with default settings) would otherwise be configured, and
    // the resulting render schedule would demand more in-flight frames
    // than the buffers hold, starving the frame buffer permanently.
    gst_pipeline_set_latency(GST_PIPELINE(output_pipeline_.get()),
                             kTargetLatency);
  }

  if (gst_element_set_state(output_pipeline_.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::println(stderr, "Could not start output pipeline");
    return false;
  }

  // Live pipelines can return NO_PREROLL. That is not an error.
  gst_element_get_state(output_pipeline_.get(), nullptr, nullptr,
                        2 * GST_SECOND);

  output_failed_.store(false);

  output_thread_ =
      std::jthread{[this](std::stop_token stop) { RunOutput(stop); }};

  if (audio_enabled_) {
    audio_output_thread_ =
        std::jthread{[this](std::stop_token stop) { RunAudioOutput(stop); }};
  }

  if (preview_enabled_) {
    preview_thread_ =
        std::jthread{[this](std::stop_token stop) { RunPreview(stop); }};
  }

  reset.release();
  return true;
}

void Stream::Implementation::RunOutput(std::stop_token stop) {
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

  // Advancing audio can't move a buffer earlier than its arrival; it is
  // realized by delaying video, keeping every render deadline feasible.
  const auto video_delay =
      audio_offset_ < 0 ? static_cast<GstClockTime>(-audio_offset_) : 0;
  OutputAnchor anchor{kTargetLatency + video_delay};

  while (!stop.stop_requested()) {
    auto frame_result = frames_.Pop(stop);

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

    GST_BUFFER_PTS(frame.get()) =
        anchor.Map(capture_pts, OutputRunningTime(pipeline, clock.get()),
                   gst_pipeline_get_latency(GST_PIPELINE(pipeline)));

    GST_BUFFER_DTS(frame.get()) = GST_CLOCK_TIME_NONE;

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
}

void Stream::Implementation::RunAudioOutput(std::stop_token stop) {
  // The casts add no references; the views are non-owning and the
  // ElementPtrs remain the sole owners.
  const auto pipeline = GstView<GstElement>{output_pipeline_.get()};
  const auto source =
      GstView<GstAppSrc>{GST_APP_SRC(output_audio_source_.get())};

  ClockPtr clock = ClockPtr{gst_element_get_clock(pipeline)};

  if (clock == nullptr) {
    std::println(stderr, "Output pipeline has no clock");

    output_failed_.store(true);
    return;
  }

  const auto audio_delay =
      audio_offset_ > 0 ? static_cast<GstClockTime>(audio_offset_) : 0;
  OutputAnchor anchor{kTargetLatency + audio_delay};

  while (!stop.stop_requested()) {
    auto chunk_result = audio_.Pop(stop);

    if (!chunk_result) {
      break;
    }

    auto chunk = std::move(*chunk_result);

    const auto capture_pts = GST_BUFFER_PTS(chunk.get());

    if (!GST_CLOCK_TIME_IS_VALID(capture_pts)) {
      std::println(stderr, "Buffered audio has no timestamp");

      output_failed_.store(true);
      break;
    }

    GST_BUFFER_PTS(chunk.get()) =
        anchor.Map(capture_pts, OutputRunningTime(pipeline, clock.get()),
                   gst_pipeline_get_latency(GST_PIPELINE(pipeline)));

    GST_BUFFER_DTS(chunk.get()) = GST_CLOCK_TIME_NONE;

    // gst_app_src_push_buffer takes ownership.
    const auto result = gst_app_src_push_buffer(source, chunk.release());

    if (result != GST_FLOW_OK) {
      if (!stop.stop_requested()) {
        std::println(stderr, "audio appsrc rejected buffer: {}",
                     static_cast<int>(result));

        output_failed_.store(true);
      }

      break;
    }
  }

  gst_app_src_end_of_stream(source);
}

void Stream::Implementation::RunPreview(std::stop_token stop) {
  const auto sink = GstView<GstAppSink>{GST_APP_SINK(preview_sink_.get())};

  while (!stop.stop_requested()) {
    SamplePtr sample =
        SamplePtr{gst_app_sink_try_pull_sample(sink, 100 * GST_MSECOND)};

    if (sample == nullptr) {
      if (gst_app_sink_is_eos(sink)) {
        break;
      }

      continue;
    }

    // gst_sample_get_buffer is transfer-none.
    const GstView<GstBuffer> encoded = gst_sample_get_buffer(sample.get());

    GstMapInfo info;

    if (encoded == nullptr ||
        !gst_buffer_map(encoded, &info, GST_MAP_READ)) {
      std::println(stderr, "Could not read encoded preview frame");
      continue;
    }

    const auto* begin = reinterpret_cast<const std::byte*>(info.data);
    auto data = std::make_shared<const std::vector<std::byte>>(
        begin, begin + info.size);

    gst_buffer_unmap(encoded, &info);

    // Never write to HTTP sockets from here: the web module waits on the
    // frame buffer and does its own I/O.
    preview_frames_.Store(GST_BUFFER_PTS(encoded), std::move(data));
  }
}

void Stream::Implementation::SetPreviewActive(bool active) {
  preview_active_.store(active);
}

void Stream::Implementation::RunScreensaver(std::stop_token stop) {
  auto next_frame = std::chrono::steady_clock::now();
  std::uint64_t frame_number = 0;
  std::uint64_t silence_sample_number = 0;

  while (!stop.stop_requested()) {
    BufferPtr frame = MakePinkFrame();

    if (frame == nullptr) {
      std::println(stderr, "Could not allocate no-signal frame");
      break;
    }

    GST_BUFFER_PTS(frame.get()) = frame_number * kFrameDuration;
    GST_BUFFER_DURATION(frame.get()) = kFrameDuration;

    ++frame_number;

    if (!frames_.PushLatest(std::move(frame))) {
      break;
    }

    if (audio_enabled_) {
      BufferPtr silence = MakeSilence();

      if (silence == nullptr) {
        std::println(stderr, "Could not allocate silence chunk");
        break;
      }

      GST_BUFFER_PTS(silence.get()) =
          silence_sample_number * GST_SECOND / kAudioRate;
      GST_BUFFER_DURATION(silence.get()) =
          kSilenceFramesPerChunk * GST_SECOND / kAudioRate;

      silence_sample_number += kSilenceFramesPerChunk;

      if (!audio_.PushLatest(std::move(silence))) {
        break;
      }
    }

    next_frame += std::chrono::nanoseconds{kFrameDuration};
    std::this_thread::sleep_until(next_frame);
  }
}

void Stream::Implementation::StopCapturePipeline(
    const std::lock_guard<std::mutex>&) {
  if (capture_thread_.joinable()) {
    capture_thread_.request_stop();
    audio_thread_.request_stop();

    // Changing state unblocks pending appsink operations.
    if (capture_pipeline_ != nullptr) {
      gst_element_set_state(capture_pipeline_.get(), GST_STATE_NULL);
    }

    capture_thread_.join();

    if (audio_thread_.joinable()) {
      audio_thread_.join();
    }

    // No partial capture audio may survive into the no-signal screen (or
    // a restarted capture): drop whatever the audio buffer still holds.
    audio_.Flush();

    ResetGuard reset{capture_pipeline_, capture_sink_, capture_audio_sink_,
                     capture_bus_};
  }

  capture_state_ = CaptureState::kStopped;
}

void Stream::Implementation::StopOutputPipeline(
    const std::lock_guard<std::mutex>&) {
  if (output_thread_.joinable()) {
    output_thread_.request_stop();
    audio_output_thread_.request_stop();
    preview_thread_.request_stop();

    // Changing state unblocks pending appsrc and appsink operations.
    if (output_pipeline_ != nullptr) {
      gst_element_set_state(output_pipeline_.get(), GST_STATE_NULL);
    }

    output_thread_.join();

    if (audio_output_thread_.joinable()) {
      audio_output_thread_.join();
    }

    if (preview_thread_.joinable()) {
      preview_thread_.join();
    }

    ResetGuard reset{output_pipeline_, output_source_, output_audio_source_,
                     preview_sink_, preview_queue_, output_bus_};
  }
}

void Stream::Implementation::Stop() {
  std::lock_guard lock{mutex_};

  frames_.Close();
  audio_.Close();
  StopCapturePipeline(lock);
  StopOutputPipeline(lock);
}

void Stream::Implementation::Poll() {
  std::lock_guard lock{mutex_};

  const bool capture_bus_ok = PollBus(capture_bus_, "capture");
  PollBus(output_bus_, "output");

  if (capture_state_ == CaptureState::kCapturing &&
      (!capture_bus_ok || !capture_active_.load())) {
    // Capture is gone; drop the pipeline and switch the capture thread
    // over to the no-signal screen.
    StopCapturePipeline(lock);
  }

  if (capture_state_ == CaptureState::kStopped) {
    capture_state_ = CaptureState::kScreensaver;

    capture_thread_ =
        std::jthread{[this](std::stop_token stop) { RunScreensaver(stop); }};
  }
}

bool Stream::Implementation::Initialize(
    const std::string& device, OutputMode output_mode,
    std::optional<int> connector_id, bool audio,
    const std::optional<std::string>& audio_output_device,
    std::int64_t audio_offset_ms, bool preview) {
  audio_enabled_ = audio;
  audio_offset_ = audio_offset_ms * GST_MSECOND;
  preview_enabled_ = preview;

  if (preview_enabled_) {
    if (auto placeholder = EncodePlaceholderJpeg()) {
      preview_frames_.Store(0, std::move(placeholder));
    } else {
      std::println(stderr, "Could not encode the preview placeholder frame");
    }
  }
  if (audio_enabled_) {
    // Not value_or: the default must not be probed when a device is given.
    audio_output_device_ =
        audio_output_device ? *audio_output_device : DefaultAudioOutputDevice();
  }

  const std::optional<std::string_view> branch_device =
      audio_enabled_
          ? std::make_optional<std::string_view>(audio_output_device_)
          : std::nullopt;

  std::println(
      "Capture pipeline:\n{}\n\nOutput pipeline:\n{}\n",
      CapturePipelineDescription(device, audio_enabled_),
      OutputPipelineDescription(output_mode, connector_id, branch_device,
                                preview_enabled_));

  return StartOutput(output_mode, connector_id) && StartCapture(device);
}

Stream::~Stream() = default;

/* static */
std::unique_ptr<Stream> Stream::Create(
    const std::string& device, OutputMode output_mode,
    std::optional<int> connector_id, bool audio,
    const std::optional<std::string>& audio_output_device,
    std::int64_t audio_offset_ms, bool preview) {
  static bool gst_initialized = false;
  if (!gst_initialized) {
    gst_initialized = true;
    gst_init(nullptr, nullptr);
  }

  // The counter-stream delay from --audio-offset needs the extra frames
  // (or ~10 ms audio chunks) in flight; make room for them.
  const auto extra_video_frames =
      static_cast<std::size_t>((audio_offset_ms < 0 ? -audio_offset_ms : 0) *
                                   kFramesPerSecond +
                               999) /
      1000;
  const auto extra_audio_chunks =
      static_cast<std::size_t>(audio_offset_ms > 0 ? audio_offset_ms : 0) / 10;

  auto implementation = std::make_unique<Implementation>(
      kFrameBufferCapacity + extra_video_frames,
      kAudioBufferCapacity + extra_audio_chunks);
  if (!implementation->Initialize(device, output_mode, connector_id, audio,
                                  audio_output_device, audio_offset_ms,
                                  preview)) {
    return nullptr;
  }

  auto stream = std::make_unique<Stream>();
  stream->implementation_ = std::move(implementation);
  return stream;
}

void Stream::Poll() { implementation_->Poll(); }

void Stream::Stop() { implementation_->Stop(); }

PreviewFrameBuffer& Stream::PreviewFrames() {
  return implementation_->PreviewFrames();
}

void Stream::SetPreviewActive(bool active) {
  implementation_->SetPreviewActive(active);
}

bool Stream::RestartCapture(const std::string& device) {
  return implementation_->StartCapture(device);
}

bool Stream::RestartOutput(OutputMode output_mode,
                           std::optional<int> connector_id) {
  return implementation_->StartOutput(output_mode, connector_id);
}

bool Stream::Failed() const { return implementation_->Failed(); }

std::uint64_t Stream::DroppedFrames() const {
  return implementation_->DroppedFrames();
}

}  // namespace subtitler
