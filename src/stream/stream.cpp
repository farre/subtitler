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
#include "stream/preview_gate.h"
#include "utils/logging.h"
#include "utils/reset_guard.h"

namespace {

using namespace subtitler;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFramesPerSecond = 60;
constexpr auto kFrameDuration = GST_SECOND / kFramesPerSecond;
constexpr std::size_t kFrameBufferCapacity = 8;

// CV105 audio: S16_LE 48 kHz stereo only (docs/pi-setup.md). Bit-transparent
// passthrough: no element may touch the samples.
constexpr std::string_view kAudioDevice = "hw:CARD=Video,DEV=0";
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr std::uint64_t kAudioBytesPerFrame = kAudioChannels * 2;
constexpr std::size_t kAudioBufferCapacity = 16;
// One silence chunk per video frame period.
constexpr std::uint64_t kSilenceFramesPerChunk = kAudioRate / kFramesPerSecond;
constexpr auto kAudioChunkDuration =
    kSilenceFramesPerChunk * GST_SECOND / kAudioRate;

// Time-based in-flight limit for the output appsrcs: comfortably above
// the worst-case computed latency so the render schedule never starves
// the sources — the starvation that made pinning latency necessary
// (#128).
constexpr GstClockTime kAppSrcMaxQueueTime = 300 * GST_MSECOND;

// Latency reported through the output appsrcs for the delay hidden behind
// the appsink/appsrc boundary: one frame/chunk of capture-side queueing
// at minimum, a full frame buffer at most. Refined by on-device
// measurement (#437).
constexpr GstClockTime kVideoMinLatency = kFrameDuration;
constexpr GstClockTime kAudioMinLatency = 10 * GST_MSECOND;

// Small alsasink ring buffer so the automatically computed pipeline
// latency stays low; a default alsasink reports ~200 ms, which no low
// latency target can honor. Microseconds: the unit of alsasink's
// buffer-time and latency-time. Must survive on-device xrun testing
// (#437).
constexpr gint64 kAudioSinkBufferTimeUs = 40000;
constexpr gint64 kAudioSinkLatencyTimeUs = 10000;

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

  STREAM_LOG(LogLevel::kError, "{} pipeline error from {}: {}", pipeline_name,
             GST_OBJECT_NAME(message->src),
             error != nullptr ? error->message : "unknown error");

  if (debug != nullptr) {
    STREAM_LOG(LogLevel::kError, "Debug information: {}", debug.get());
  }
}

bool PollBus(GstView<GstBus> bus, GstView<GstElement> pipeline,
             std::string_view pipeline_name) {
  if (bus == nullptr) {
    return true;
  }

  bool ok = true;

  while (auto message = MessagePtr{gst_bus_pop(bus)}) {
    switch (GST_MESSAGE_TYPE(message.get())) {
      case GST_MESSAGE_ERROR:
        PrintBusError(pipeline_name, message);
        ok = false;
        break;
      case GST_MESSAGE_EOS:
        STREAM_LOG(LogLevel::kInfo, "{} pipeline reached EOS", pipeline_name);
        ok = false;
        break;
      case GST_MESSAGE_LATENCY:
        // A sink (re)negotiated its latency; redistribute the new global
        // latency. With automatic latency this is the only way the
        // pipeline learns about it (#437).
        if (!gst_bin_recalculate_latency(GST_BIN(pipeline))) {
          STREAM_LOG(LogLevel::kError,
                     "Could not recalculate {} pipeline latency",
                     pipeline_name);
          ok = false;
        }
        break;
      default:
        break;
    }
  }

  return ok;
}

BufferPtr MakeSolidFrame(int width, int height) {
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
  ElementPtr pipeline{
      gst_parse_launch(description.c_str(), std::out_ptr(error))};

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
    sample =
        SamplePtr{gst_app_sink_try_pull_sample(app_sink, 100 * GST_MSECOND)};
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
  auto data =
      std::make_shared<const std::vector<std::byte>>(begin, begin + info.size);

  gst_buffer_unmap(encoded, &info);

  return data;
}

BufferPtr CopyCapturedBuffer(GstView<GstSample> sample) {
  // gst_sample_get_buffer is transfer-none.
  const GstView<GstBuffer> captured = gst_sample_get_buffer(sample);

  if (captured == nullptr) {
    STREAM_LOG(LogLevel::kError, "Captured sample contained no buffer");

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
    std::optional<std::string_view> audio_device, bool preview,
    std::optional<std::string_view> subtitles) {
  auto description =
      VideoOutputPipelineDescription(mode, connector_id, preview, subtitles);
  if (audio_device) {
    description += " " + AudioOutputPipelineDescription(*audio_device);
  }
  return description;
}

// The subtitle side of the output pipeline (#438): raw SRT from file into
// the subtitleoverlay's subtitle sink. subparse is plugged explicitly —
// subtitleoverlay's parser autoplugging filters on a "Parser/Subtitle"
// klass that subparse ("Codec/Decoder/Subtitle") doesn't have, while its
// renderer autoplugging special-cases textoverlay by name. The named
// parser is where the anchor lives: StartOutput maps cue times onto the
// shared capture timeline with gst_pad_set_offset on its src pad.
std::string SubtitlePipelineDescription(std::string_view path) {
  return std::format(
      "filesrc "
      "location=\"{}\" "
      "! application/x-subtitle "
      "! subparse "
      "name=subtitle_parser "
      "! subtitle_overlay.subtitle_sink",
      path);
}

// The preview side of the output tee. The leaky queue is the only coupling
// to the HDMI branch and never blocks it; the gate installed on the queue
// (InstallPreviewGate) drops all branch buffers until a web client
// activates the preview and decimates 60 fps to the preview rate while
// active, so the whole branch idles when unwatched (#384). async=false is
// load-bearing: with the gate closed the appsink starves, and a starving
// sink's preroll would otherwise block the whole pipeline (HDMI included)
// from reaching PLAYING.
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
      "! jpegenc "
      "quality={} "
      "! appsink "
      "name=preview_sink "
      "sync=false "
      "async=false "
      "max-buffers=1 "
      "drop=true",
      kPreviewWidth, kPreviewHeight, kPreviewJpegQuality);
}

std::string AudioOutputPipelineDescription(std::string_view device) {
  return std::format(
      "appsrc "
      "name=output_audio_source "
      "is-live=true "
      "format=time "
      "block=true "
      "max-buffers=0 "
      "max-bytes=0 "
      "max-time={} "
      "! alsasink "
      "device=\"{}\" "
      "buffer-time={} "
      "latency-time={} "
      "slave-method=skew",
      kAppSrcMaxQueueTime, device, kAudioSinkBufferTimeUs,
      kAudioSinkLatencyTimeUs);
}

std::string VideoOutputPipelineDescription(
    OutputMode mode, std::optional<int> connector_id, bool preview,
    std::optional<std::string_view> subtitles) {
  const auto connector = connector_id
                             ? std::format(" connector-id={}", *connector_id)
                             : std::string{};

  const std::string base = std::format(
      "appsrc "
      "name=output_source "
      "is-live=true "
      "format=time "
      "block=true "
      "max-buffers=0 "
      "max-bytes=0 "
      "max-time={} ",
      kAppSrcMaxQueueTime);

  // The overlay composites onto the 4:2:2 frame before conversion (text
  // chroma) and before the tee, so the MJPEG preview shows subtitles too
  // (docs/video-output.md).
  const std::string overlay =
      subtitles ? "! subtitleoverlay name=subtitle_overlay " : "";
  const std::string subtitle_branch =
      subtitles ? " " + SubtitlePipelineDescription(*subtitles) : "";

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
    return base + overlay + tail + subtitle_branch;
  }

  // The tee sits right after the subtitle overlay, so the preview taps
  // composited video (#379). Every tee branch needs its own queue: without
  // one the clock-synced sink blocks the tee's streaming thread in preroll
  // and the pipeline deadlocks. Sized 1 and not leaky so HDMI frames are
  // never dropped here.
  return std::format(
      "{}{}! tee name=output_tee output_tee. "
      "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 {} {}{}",
      base, overlay, tail, PreviewOutputPipelineDescription(), subtitle_branch);
}

struct Stream::Implementation {
  enum class CaptureState {
    kStopped,
    kCapturing,
    kScreensaver,
  };

  Implementation(std::size_t frame_capacity, std::size_t audio_capacity)
      : frame_capacity_{frame_capacity},
        audio_capacity_{audio_capacity},
        frames_(frame_capacity),
        audio_(audio_capacity) {}

  bool Initialize(const std::string& device, OutputMode output_mode,
                  std::optional<int> connector_id, bool audio,
                  const std::optional<std::string>& audio_output_device,
                  std::int64_t audio_offset_ms, bool preview,
                  const std::optional<std::string>& subtitles);

  ~Implementation() { Stop(); }

  bool StartCapture(const std::string& device);
  bool StartOutput(OutputMode output_mode, std::optional<int> connector_id);

  // Puts a pipeline on the shared timeline (see master_clock_).
  void ConfigureTimeline(GstView<GstElement> pipeline) const;
  GstClockTime MasterRunningTime() const;

  void Stop();

  void Poll();

  void RunCapture(std::stop_token stop);
  void RunAudioCapture(std::stop_token stop);
  void RunOutput(std::stop_token stop);
  void RunAudioOutput(std::stop_token stop);
  void RunScreensaver(std::stop_token stop);
  void RunPreview(std::stop_token stop);

  void SetPreviewActive(bool active);

  bool SetSubtitleFile(const std::optional<std::string>& path);
  void SetSubtitleDelay(std::int64_t delay_ms);
  void SetSubtitlesVisible(bool visible);

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

  // One clock and base time for every pipeline, so capture and output
  // share a running-time domain: captured PTS are valid output timestamps
  // and capture restarts stay monotonic (#437). Pinning the clock also
  // keeps the output pipeline from adopting the alsasink ring-buffer
  // clock, which starts at zero when the device starts (#128).
  const ClockPtr master_clock_{gst_system_clock_obtain()};
  const GstClockTime master_base_time_ =
      gst_clock_get_time(master_clock_.get());

  const std::size_t frame_capacity_;
  const std::size_t audio_capacity_;

  FrameBuffer frames_;
  FrameBuffer audio_;

  std::atomic_bool capture_active_ = false;
  std::atomic_bool capture_failed_ = false;
  std::atomic_bool output_failed_ = false;

  bool audio_enabled_ = false;
  std::string audio_output_device_;
  // Signed nanoseconds shifting audio relative to video; negative values
  // are realized as a video delay.
  std::int64_t audio_offset_ = 0;

  // Remembered for SetSubtitleFile's output rebuild.
  OutputMode output_mode_ = OutputMode::kKmsSoftware;
  std::optional<int> connector_id_;

  // The SRT rendered by the output pipeline's subtitle branch, if any.
  // Guarded by mutex_.
  std::optional<std::string> subtitle_path_;
  // Live trim added to the anchor: cue at SRT time t renders at
  // subtitle_anchor_ + delay + t. Signed nanoseconds; positive delays
  // cues. Written by SetSubtitleDelay (any thread), applied under mutex_.
  std::atomic<std::int64_t> subtitle_delay_ = 0;
  // The output running time at which SRT t=0 renders; set at every
  // StartOutput so file switches replay from "now" (#438).
  GstClockTime subtitle_anchor_ = 0;
  // Applies subtitle_anchor_ + subtitle_delay_ as the parser's pad
  // offset. Call with mutex_ held.
  void ApplySubtitleOffset();

  // Set at Initialize: whether the output pipeline has a preview branch.
  bool preview_enabled_ = false;
  // Shared with the preview gate's pad probe on the streaming thread.
  PreviewGate preview_gate_{kFramesPerSecond / kPreviewFramesPerSecond};
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
  ElementPtr subtitle_overlay_;
  ElementPtr subtitle_parser_;
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

void Stream::Implementation::ConfigureTimeline(
    GstView<GstElement> pipeline) const {
  gst_pipeline_use_clock(GST_PIPELINE(pipeline), master_clock_.get());

  // The application owns start/base time; nothing resets the domain.
  gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time(pipeline, master_base_time_);
}

GstClockTime Stream::Implementation::MasterRunningTime() const {
  const auto now = gst_clock_get_time(master_clock_.get());
  return now >= master_base_time_ ? now - master_base_time_ : 0;
}

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

  ConfigureTimeline(capture_pipeline_.get());

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

  GstClockTime fallback_pts = GST_CLOCK_TIME_NONE;

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
      STREAM_LOG(LogLevel::kError, "Could not copy captured frame");

      capture_failed_.store(true);
      break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(copied.get()))) {
      if (!GST_CLOCK_TIME_IS_VALID(fallback_pts)) {
        fallback_pts = MasterRunningTime();
      }
      GST_BUFFER_PTS(copied.get()) = fallback_pts;
    }

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(copied.get()))) {
      GST_BUFFER_DURATION(copied.get()) = kFrameDuration;
    }

    fallback_pts = GST_BUFFER_PTS(copied.get()) + kFrameDuration;

    if (!frames_.PushLatest(std::move(copied))) {
      break;
    }
  }

  capture_active_.store(false);
}

void Stream::Implementation::RunAudioCapture(std::stop_token stop) {
  const auto sink =
      GstView<GstAppSink>{GST_APP_SINK(capture_audio_sink_.get())};

  GstClockTime fallback_pts = GST_CLOCK_TIME_NONE;

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
      STREAM_LOG(LogLevel::kError, "Could not copy captured audio");

      capture_failed_.store(true);
      break;
    }

    const auto frames = gst_buffer_get_size(copied.get()) / kAudioBytesPerFrame;

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(copied.get()))) {
      if (!GST_CLOCK_TIME_IS_VALID(fallback_pts)) {
        fallback_pts = MasterRunningTime();
      }
      GST_BUFFER_PTS(copied.get()) = fallback_pts;
    }

    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(copied.get()))) {
      GST_BUFFER_DURATION(copied.get()) = frames * GST_SECOND / kAudioRate;
    }

    fallback_pts =
        GST_BUFFER_PTS(copied.get()) + GST_BUFFER_DURATION(copied.get());

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

  output_mode_ = output_mode;
  connector_id_ = connector_id;

  ResetGuard reset{output_pipeline_, output_source_, output_audio_source_,
                   preview_sink_,    preview_queue_, subtitle_overlay_,
                   subtitle_parser_, output_bus_};

  const std::optional<std::string_view> audio_device =
      audio_enabled_
          ? std::make_optional<std::string_view>(audio_output_device_)
          : std::nullopt;

  output_pipeline_ = ParsePipeline(
      "output",
      OutputPipelineDescription(output_mode, connector_id, audio_device,
                                preview_enabled_, subtitle_path_));

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

    // Tell the latency query what hides behind the appsink/appsrc
    // boundary: the output pipeline can't see the capture side, and an
    // undercut latency budget makes sinks drop buffers as late (#437).
    g_object_set(output_source_.get(), "min-latency",
                 static_cast<gint64>(kVideoMinLatency), "max-latency",
                 static_cast<gint64>(frame_capacity_ * kFrameDuration),
                 nullptr);
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

    g_object_set(output_audio_source_.get(), "min-latency",
                 static_cast<gint64>(kAudioMinLatency), "max-latency",
                 static_cast<gint64>(audio_capacity_ * kAudioChunkDuration),
                 nullptr);
  }

  if (preview_enabled_) {
    preview_sink_ = ElementPtr{
        gst_bin_get_by_name(GST_BIN(output_pipeline_.get()), "preview_sink")};
    preview_queue_ = ElementPtr{
        gst_bin_get_by_name(GST_BIN(output_pipeline_.get()), "preview_queue")};

    if (!preview_sink_ || !preview_queue_) {
      std::println(stderr, "Couldn't find preview branch elements");
      return false;
    }

    // The gate drops all preview branch buffers while there are no web
    // clients, so no JPEG encoding happens without watchers (#384).
    InstallPreviewGate(preview_queue_.get(), preview_gate_);
  }

  if (subtitle_path_) {
    subtitle_overlay_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "subtitle_overlay")};
    subtitle_parser_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "subtitle_parser")};

    if (!subtitle_overlay_ || !subtitle_parser_) {
      std::println(stderr, "Couldn't find subtitle branch elements");
      return false;
    }

    // Anchor the SRT timeline at the current running time: cue time t
    // renders at anchor + delay + t (#438).
    subtitle_anchor_ = MasterRunningTime();
    ApplySubtitleOffset();
  }

  output_bus_ = BusPtr{gst_element_get_bus(output_pipeline_.get())};

  ConfigureTimeline(output_pipeline_.get());

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
  // The cast adds no reference; the view is non-owning and the
  // ElementPtr remains the sole owner.
  const auto source = GstView<GstAppSrc>{GST_APP_SRC(output_source_.get())};

  // Advancing audio can't move a buffer earlier than its arrival; it is
  // realized by delaying video, keeping every render deadline feasible.
  const auto delay =
      audio_offset_ < 0 ? static_cast<GstClockTime>(-audio_offset_) : 0;

  std::uint64_t generation = 0;

  while (!stop.stop_requested()) {
    auto frame_result = frames_.Pop(stop);

    if (!frame_result) {
      break;
    }

    auto frame = std::move(*frame_result);

    const auto capture_pts = GST_BUFFER_PTS(frame.get());

    if (!GST_CLOCK_TIME_IS_VALID(capture_pts)) {
      STREAM_LOG(LogLevel::kError, "Buffered frame has no timestamp");

      output_failed_.store(true);
      break;
    }

    // Capture and output share a running-time domain, so the captured
    // PTS is already an output timestamp (#437).
    GST_BUFFER_PTS(frame.get()) = capture_pts + delay;
    GST_BUFFER_DTS(frame.get()) = GST_CLOCK_TIME_NONE;

    // The first buffer after a frame-buffer flush is not contiguous with
    // what the sink last saw.
    if (const auto current = frames_.Generation(); current != generation) {
      generation = current;
      GST_BUFFER_FLAG_SET(frame.get(), GST_BUFFER_FLAG_DISCONT);
    }

    // gst_app_src_push_buffer takes ownership.
    const auto result = gst_app_src_push_buffer(source, frame.release());

    if (result != GST_FLOW_OK) {
      if (!stop.stop_requested()) {
        STREAM_LOG(LogLevel::kError, "appsrc rejected frame: {}",
                   static_cast<int>(result));

        output_failed_.store(true);
      }

      break;
    }
  }

  gst_app_src_end_of_stream(source);
}

void Stream::Implementation::RunAudioOutput(std::stop_token stop) {
  // The cast adds no reference; the view is non-owning and the
  // ElementPtr remains the sole owner.
  const auto source =
      GstView<GstAppSrc>{GST_APP_SRC(output_audio_source_.get())};

  const auto delay =
      audio_offset_ > 0 ? static_cast<GstClockTime>(audio_offset_) : 0;

  std::uint64_t generation = 0;

  while (!stop.stop_requested()) {
    auto chunk_result = audio_.Pop(stop);

    if (!chunk_result) {
      break;
    }

    auto chunk = std::move(*chunk_result);

    const auto capture_pts = GST_BUFFER_PTS(chunk.get());

    if (!GST_CLOCK_TIME_IS_VALID(capture_pts)) {
      STREAM_LOG(LogLevel::kError, "Buffered audio has no timestamp");

      output_failed_.store(true);
      break;
    }

    GST_BUFFER_PTS(chunk.get()) = capture_pts + delay;
    GST_BUFFER_DTS(chunk.get()) = GST_CLOCK_TIME_NONE;

    if (const auto current = audio_.Generation(); current != generation) {
      generation = current;
      GST_BUFFER_FLAG_SET(chunk.get(), GST_BUFFER_FLAG_DISCONT);
    }

    // gst_app_src_push_buffer takes ownership.
    const auto result = gst_app_src_push_buffer(source, chunk.release());

    if (result != GST_FLOW_OK) {
      if (!stop.stop_requested()) {
        STREAM_LOG(LogLevel::kError, "audio appsrc rejected buffer: {}",
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

    if (encoded == nullptr || !gst_buffer_map(encoded, &info, GST_MAP_READ)) {
      STREAM_LOG(LogLevel::kWarning, "Could not read encoded preview frame");
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

void Stream::Implementation::ApplySubtitleOffset() {
  if (subtitle_parser_ == nullptr) {
    return;
  }

  // A pad offset shifts the running time of everything leaving the
  // parser, live-adjustable: textoverlay compares text and video by
  // running time, so cue time t lands on anchor + delay + t (#438).
  PadPtr pad{gst_element_get_static_pad(subtitle_parser_.get(), "src")};
  gst_pad_set_offset(pad.get(), static_cast<gint64>(subtitle_anchor_) +
                                    subtitle_delay_.load());
}

void Stream::Implementation::SetPreviewActive(bool active) {
  preview_gate_.active.store(active);
}

bool Stream::Implementation::SetSubtitleFile(
    const std::optional<std::string>& path) {
  OutputMode output_mode;
  std::optional<int> connector_id;
  {
    std::lock_guard lock{mutex_};
    subtitle_path_ = path;
    subtitle_delay_.store(0);
    output_mode = output_mode_;
    connector_id = connector_id_;
  }

  return StartOutput(output_mode, connector_id);
}

void Stream::Implementation::SetSubtitleDelay(std::int64_t delay_ms) {
  std::lock_guard lock{mutex_};
  subtitle_delay_.store(delay_ms * GST_MSECOND);
  ApplySubtitleOffset();
}

void Stream::Implementation::SetSubtitlesVisible(bool visible) {
  std::lock_guard lock{mutex_};
  if (subtitle_overlay_ != nullptr) {
    g_object_set(subtitle_overlay_.get(), "silent", !visible, nullptr);
  }
}

void Stream::Implementation::RunScreensaver(std::stop_token stop) {
  auto next_frame = std::chrono::steady_clock::now();
  // Timestamp in the shared running time: no-signal buffers flow through
  // the same output timeline as captured ones, so capture/screensaver
  // transitions stay monotonic (#437).
  auto next_pts = MasterRunningTime();

  while (!stop.stop_requested()) {
    BufferPtr frame = MakePinkFrame();

    if (frame == nullptr) {
      STREAM_LOG(LogLevel::kError, "Could not allocate no-signal frame");
      break;
    }

    GST_BUFFER_PTS(frame.get()) = next_pts;
    GST_BUFFER_DURATION(frame.get()) = kFrameDuration;

    if (!frames_.PushLatest(std::move(frame))) {
      break;
    }

    if (audio_enabled_) {
      BufferPtr silence = MakeSilence();

      if (silence == nullptr) {
        STREAM_LOG(LogLevel::kError, "Could not allocate silence chunk");
        break;
      }

      GST_BUFFER_PTS(silence.get()) = next_pts;
      GST_BUFFER_DURATION(silence.get()) = kAudioChunkDuration;

      if (!audio_.PushLatest(std::move(silence))) {
        break;
      }
    }

    next_pts += kFrameDuration;
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

    // No partial capture data may survive into the no-signal screen (or
    // a restarted capture): drop whatever the frame buffers still hold.
    frames_.Flush();
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
                     preview_sink_,    preview_queue_, subtitle_overlay_,
                     subtitle_parser_, output_bus_};
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

  const bool capture_bus_ok =
      PollBus(capture_bus_.get(), capture_pipeline_.get(), "capture");
  PollBus(output_bus_.get(), output_pipeline_.get(), "output");

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
    std::int64_t audio_offset_ms, bool preview,
    const std::optional<std::string>& subtitles) {
  audio_enabled_ = audio;
  audio_offset_ = audio_offset_ms * GST_MSECOND;
  preview_enabled_ = preview;
  subtitle_path_ = subtitles;

  if (preview_enabled_) {
    if (auto placeholder = EncodePlaceholderJpeg()) {
      preview_frames_.Store(0, std::move(placeholder));
    } else {
      STREAM_LOG(LogLevel::kError,
                 "Could not encode the preview placeholder frame");
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

  STREAM_LOG(LogLevel::kDebug, "Capture pipeline:\n{}\n\nOutput pipeline:\n{}",
             CapturePipelineDescription(device, audio_enabled_),
             OutputPipelineDescription(output_mode, connector_id, branch_device,
                                       preview_enabled_, subtitle_path_));

  return StartOutput(output_mode, connector_id) && StartCapture(device);
}

Stream::~Stream() = default;

/* static */
std::unique_ptr<Stream> Stream::Create(
    const std::string& device, OutputMode output_mode,
    std::optional<int> connector_id, bool audio,
    const std::optional<std::string>& audio_output_device,
    std::int64_t audio_offset_ms, bool preview,
    const std::optional<std::string>& subtitles) {
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
                                  audio_output_device, audio_offset_ms, preview,
                                  subtitles)) {
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

bool Stream::SetSubtitleFile(const std::optional<std::string>& path) {
  return implementation_->SetSubtitleFile(path);
}

void Stream::SetSubtitleDelay(std::int64_t delay_ms) {
  implementation_->SetSubtitleDelay(delay_ms);
}

void Stream::SetSubtitlesVisible(bool visible) {
  implementation_->SetSubtitlesVisible(visible);
}

bool Stream::Failed() const { return implementation_->Failed(); }

std::uint64_t Stream::DroppedFrames() const {
  return implementation_->DroppedFrames();
}

}  // namespace subtitler
