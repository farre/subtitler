#include "stream/stream.h"

#include <alsa/asoundlib.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
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
#include "stream/drop_gate.h"
#include "stream/frame_buffer.h"
#include "stream/sync_matcher.h"
#include "stream/sync_session.h"
#include "stream/whisper_transcriber.h"
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

// The whisper tap's sample rate: the only rate whisper consumes.
constexpr int kWhisperRate = 16000;

// The one-shot sync session's listening window (#433): long enough for
// several whisper windows of dialogue, short enough that a failed sync
// is answered promptly. Nanoseconds, the shared-timeline unit.
constexpr std::int64_t kSyncListenWindowNs = 45'000'000'000;

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

std::string CapturePipelineDescription(std::string_view device, bool audio,
                                       bool whisper) {
  auto description = VideoCapturePipelineDescription(device);
  if (audio) {
    description += " " + AudioCapturePipelineDescription(kAudioDevice, whisper);
    if (whisper) {
      description += " " + WhisperCapturePipelineDescription();
    }
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

std::string AudioCapturePipelineDescription(std::string_view device,
                                            bool whisper) {
  // A single linear chain so that a tee for the whisper tap (#19) can be
  // inserted without touching the passthrough path. Like the output
  // tee's HDMI branch, the passthrough side of the tee gets its own
  // non-leaky queue: without it the two sinks' preroll waits serialize
  // through the tee and deadlock the live source's PLAYING transition
  // (covered by the "whisper tap flows converted audio" test).
  return std::format(
      "alsasrc "
      "device=\"{}\" "
      "do-timestamp=true "
      "! audio/x-raw,"
      "format=S16LE,"
      "rate={},"
      "channels={} "
      "{}"
      "! appsink "
      "name=capture_audio_sink "
      "sync=false "
      "max-buffers=8 "
      "drop=true",
      device, kAudioRate, kAudioChannels,
      whisper ? "! tee name=whisper_tee "
                "! queue "
                "max-size-buffers=8 "
                "max-size-bytes=0 "
                "max-size-time=0 "
              : "");
}

std::string WhisperCapturePipelineDescription() {
  // The whisper tap (#19). The leaky queue decouples the branch from
  // the tee (every tee branch needs its own queue — see the passthrough
  // comment) and drops backlog older than a couple of seconds; the
  // dropping appsink is the backstop. Between them the recognition lag
  // stays bounded while a window is being transcribed, and inference
  // can never stall the passthrough. async=false is load-bearing: the
  // branch starts gated (DropGate drops every buffer), and a starving
  // sink's preroll would otherwise hold the whole pipeline in PAUSED,
  // passthrough included — the same rule as the preview branch.
  return std::format(
      "whisper_tee. "
      "! queue "
      "name=whisper_queue "
      "max-size-buffers=0 "
      "max-size-bytes=0 "
      "max-size-time=2000000000 "
      "leaky=downstream "
      "! audioconvert "
      "! audioresample "
      "! audio/x-raw,"
      "format=F32LE,"
      "rate={},"
      "channels=1 "
      "! appsink "
      "name=whisper_sink "
      "sync=false "
      "async=false "
      "max-buffers=300 "
      "drop=true",
      kWhisperRate);
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
// shared capture timeline with gst_pad_set_offset on its src pad. The
// offset reaches only cues parsed after the change, so position and
// delay changes flush-seek subtitle_source back to the start and let
// subparse re-emit through the new offset (#439).
std::string SubtitlePipelineDescription(std::string_view path) {
  return std::format(
      "filesrc "
      "name=subtitle_source "
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
                  const std::optional<std::string>& subtitles,
                  const std::optional<std::string>& whisper_model);

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
  void RunWhisper(std::stop_token stop);

  void SetPreviewActive(bool active);

  bool SetWhisperState(bool enabled,
                       const std::optional<std::string>& model_path);
  bool WhisperEnabled() const { return whisper_enabled_.load(); }
  std::optional<std::string> WhisperModel() const;

  SyncStartResult StartSubtitleSync();
  Stream::SyncState SubtitleSync() const;

  bool SetSubtitleFile(const std::optional<std::string>& path);
  void SetSubtitleDelay(std::int64_t delay_ms);
  void SetSubtitlesVisible(bool visible);
  std::optional<std::int64_t> SubtitleTime();
  void SetSubtitleTime(std::int64_t time_ms);
  // Moves the SRT position; call with mutex_ held.
  void ApplySubtitleTimeLocked(std::int64_t time_ms);
  // Drops the sync session and resets its public state; call with
  // mutex_ held.
  void CancelSyncLocked();
  void SetSubtitlesPaused(bool paused);
  bool SubtitlesPaused();
  bool SubtitlesVisible();
  std::int64_t SubtitleDelay();
  void SetSubtitleFontFamily(std::string family);
  void SetSubtitleFontSize(std::int64_t size_pt);
  void SetSubtitleFontColor(std::uint32_t color_argb);
  std::optional<std::string> SubtitleFontFamily() const;
  std::optional<std::int64_t> SubtitleFontSize() const;
  std::optional<std::uint32_t> SubtitleFontColor() const;

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
  // StartOutput so file switches replay from "now" (#438). Signed:
  // SetSubtitleTime can move it before the time origin. Guarded by mutex_.
  gint64 subtitle_anchor_ = 0;
  // The frozen SRT position in nanoseconds while paused; nullopt while
  // playing. Guarded by mutex_.
  std::optional<gint64> subtitle_frozen_;
  // The SetSubtitlesVisible state, re-applied to the overlay at every
  // output (re)start so it survives rebuilds. Guarded by mutex_.
  bool subtitles_visible_ = true;
  // The cue text style (#159); a field stays nullopt until its
  // SetSubtitleFont* runs. An atomic snapshot because the child-added
  // handler reads it from a streaming thread, where mutex_ must not be
  // taken (StartOutput holds it while the pipeline prerolls).
  struct SubtitleStyle {
    std::optional<std::string> family;
    std::optional<std::int64_t> size_pt;
    // Big-endian ARGB; 0xFFRRGGBB for an opaque #RRGGBB.
    std::optional<std::uint32_t> color_argb;
  };
  std::atomic<std::shared_ptr<const SubtitleStyle>> subtitle_style_;
  // The overlay's auto-plugged text renderer, its GstChildProxy child
  // named "renderer"; null until the pipeline runs.
  ElementPtr SubtitleRenderer() const;
  // Applies the stored style to the renderer, composing a Pango
  // font-desc (unset fields keep the renderer's defaults; "Sans 24",
  // "Serif", and "24" all parse). Safe to call from any thread.
  void ApplySubtitleStyle(GstElement* renderer) const;
  static void OnSubtitleChildAdded(GstChildProxy*, GObject* child,
                                   gchar* name, gpointer user_data);
  // Applies subtitle_anchor_ + subtitle_delay_ as the parser's pad
  // offset. The offset reaches only cues parsed after the change, so
  // position and delay changes must follow up with ReparseSubtitles.
  // Call with mutex_ held.
  void ApplySubtitleOffset();
  // Flush-seeks the subtitle branch back to the start so subparse
  // re-emits every cue through the current pad offset. Call with mutex_
  // held.
  void ReparseSubtitles();

  // The whisper tap (#19). The branch is in the capture pipeline
  // whenever audio is enabled and whisper is compiled in, gated by
  // whisper_gate_: enabling flips the gate and loads the model,
  // disabling closes the gate and unloads it — the passthrough is never
  // rebuilt. The transcriber is a shared_ptr because the web thread
  // swaps it under whisper_mutex_ while whisper_thread_ keeps a copy
  // for its current window; the gate is flipped after the swap so no
  // buffer meets a null transcriber.
  std::atomic_bool whisper_enabled_ = false;
  std::atomic<std::shared_ptr<WhisperTranscriber>> whisper_transcriber_;
  mutable std::mutex whisper_mutex_;

  // The one-shot sync session (#433). Guarded by sync_mutex_, which is
  // always taken after mutex_ (Poll applies a lock) or whisper_mutex_
  // (whisper disable), never the other way. The whisper thread feeds
  // windows; Poll applies a lock's position (a direct anchor set, since
  // Poll already holds mutex_).
  mutable std::mutex sync_mutex_;
  std::optional<SyncSession> sync_session_;
  Stream::SyncState sync_state_;
  bool sync_apply_pending_ = false;
  std::string whisper_model_path_;
  DropGate whisper_gate_{1};

  // Set at Initialize: whether the output pipeline has a preview branch.
  bool preview_enabled_ = false;
  // Shared with the preview gate's pad probe on the streaming thread.
  DropGate preview_gate_{kFramesPerSecond / kPreviewFramesPerSecond};
  PreviewFrameBuffer preview_frames_;

  CaptureState capture_state_ = CaptureState::kStopped;

  ElementPtr capture_pipeline_;
  ElementPtr capture_sink_;
  ElementPtr capture_audio_sink_;
  ElementPtr whisper_sink_;
  ElementPtr whisper_queue_;
  BusPtr capture_bus_;

  ElementPtr output_pipeline_;
  ElementPtr output_source_;
  ElementPtr output_audio_source_;
  ElementPtr preview_sink_;
  ElementPtr preview_queue_;
  ElementPtr subtitle_overlay_;
  ElementPtr subtitle_parser_;
  ElementPtr subtitle_source_;
  BusPtr output_bus_;

  // Runs either the capture or the screensaver loop, never both.
  std::jthread capture_thread_;
  // Drains the audio branch while capturing; joinable only when audio is
  // enabled.
  std::jthread audio_thread_;
  // Drains the whisper tap's appsink into whisper_transcriber_; joinable
  // only while capturing with the whisper tap enabled.
  std::jthread whisper_thread_;
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

  ResetGuard reset{capture_pipeline_, capture_sink_,  capture_audio_sink_,
                   whisper_sink_,     whisper_queue_, capture_bus_};

  // The whisper tap branch is present whenever it can be enabled at
  // runtime; its gate starts closed, so a present-but-disabled tap
  // costs a tee and a leaky queue, nothing more.
  const bool whisper_branch = audio_enabled_ && kWhisperAvailable;

  capture_pipeline_ = ParsePipeline(
      "capture",
      CapturePipelineDescription(device, audio_enabled_, whisper_branch));

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

  if (whisper_branch) {
    whisper_sink_ = ElementPtr{
        gst_bin_get_by_name(GST_BIN(capture_pipeline_.get()), "whisper_sink")};
    whisper_queue_ = ElementPtr{
        gst_bin_get_by_name(GST_BIN(capture_pipeline_.get()), "whisper_queue")};

    if (!whisper_sink_ || !whisper_queue_) {
      std::println(stderr, "Couldn't find the whisper tap's elements");
      return false;
    }

    InstallDropGate(whisper_queue_.get(), whisper_gate_);
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

  if (whisper_branch) {
    whisper_thread_ =
        std::jthread{[this](std::stop_token stop) { RunWhisper(stop); }};
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

void Stream::Implementation::RunWhisper(std::stop_token stop) {
  const auto sink = GstView<GstAppSink>{GST_APP_SINK(whisper_sink_.get())};

  // The running time the current window's audio ends at: the last
  // buffer's PTS plus duration (do-timestamp stamps in the shared
  // timeline domain). Windows with drops in them end slightly early;
  // the vote absorbs it.
  GstClockTime window_end = GST_CLOCK_TIME_NONE;

  while (!stop.stop_requested()) {
    SamplePtr sample =
        SamplePtr{gst_app_sink_try_pull_sample(sink, 100 * GST_MSECOND)};

    if (sample == nullptr) {
      if (gst_app_sink_is_eos(sink)) {
        break;
      }

      continue;
    }

    // Buffers can arrive while the gate drains the backlog queued
    // before a disable; drop them with no transcriber.
    const auto transcriber = whisper_transcriber_.load();
    if (transcriber == nullptr) {
      continue;
    }

    // gst_sample_get_buffer is transfer-none.
    const GstView<GstBuffer> buffer = gst_sample_get_buffer(sample.get());

    GstMapInfo info;

    if (buffer == nullptr || !gst_buffer_map(buffer, &info, GST_MAP_READ)) {
      continue;
    }

    if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
      window_end = GST_BUFFER_PTS(buffer);
      if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(buffer))) {
        window_end += GST_BUFFER_DURATION(buffer);
      }
    }

    // The tap's caps pin F32LE; buffers hold whole frames.
    const std::span<const float> samples{
        reinterpret_cast<const float*>(info.data), info.size / sizeof(float)};

    auto text = transcriber->Push(samples);

    gst_buffer_unmap(buffer, &info);

    if (!text) {
      continue;
    }

    if (!text->empty()) {
      STREAM_LOG(LogLevel::kInfo, "Whisper heard: {}", *text);
    }

    const auto audio_end = GST_CLOCK_TIME_IS_VALID(window_end)
                               ? static_cast<std::int64_t>(window_end)
                               : static_cast<std::int64_t>(MasterRunningTime());

    std::lock_guard sync_lock{sync_mutex_};
    if (sync_session_) {
      const auto result = sync_session_->Feed({std::move(*text), audio_end},
                                              MasterRunningTime());

      if (result.state == SyncSession::State::kSynced) {
        STREAM_LOG(LogLevel::kInfo, "Subtitle sync locked at {} ms",
                   *result.time_ms);
        sync_state_.status = SyncStatus::kSynced;
        sync_state_.time_ms = result.time_ms;
        // Poll applies the position: it already holds mutex_ for the
        // anchor and re-parse, this thread must not take it.
        sync_apply_pending_ = true;
        sync_session_.reset();
      } else if (result.state == SyncSession::State::kFailed) {
        STREAM_LOG(LogLevel::kInfo, "Subtitle sync failed: {}", result.reason);
        sync_state_.status = SyncStatus::kFailed;
        sync_state_.reason = result.reason;
        sync_session_.reset();
      }
    }
  }
}

bool Stream::Implementation::SetWhisperState(
    bool enabled, const std::optional<std::string>& model_path) {
  std::lock_guard lock{whisper_mutex_};

  if (!enabled) {
    // Gate first: nothing new reaches the transcriber, then the model
    // can go (the whisper thread's copy keeps its window alive).
    whisper_enabled_.store(false);
    whisper_gate_.active.store(false, std::memory_order_relaxed);
    whisper_transcriber_.store({});
    // Selecting the next model works while disabled; it is loaded on
    // the next enable.
    if (model_path) {
      whisper_model_path_ = *model_path;
    }

    // A listening session depends on the tap.
    std::lock_guard sync_lock{sync_mutex_};
    if (sync_session_) {
      sync_state_.status = SyncStatus::kFailed;
      sync_state_.reason = "whisper disabled";
      sync_session_.reset();
    }
    return true;
  }

  if (!audio_enabled_) {
    return false;
  }

  const std::string path = model_path.value_or(whisper_model_path_);
  if (path.empty()) {
    return false;
  }

  if (path == whisper_model_path_ && whisper_transcriber_.load() != nullptr) {
    // Already running this model; just re-open the gate.
    whisper_enabled_.store(true);
    whisper_gate_.active.store(true, std::memory_order_relaxed);
    return true;
  }

  auto transcriber = WhisperTranscriber::Create(path);
  if (!transcriber) {
    return false;
  }

  whisper_model_path_ = path;
  // The transcriber before the gate, so no buffer meets a null one.
  whisper_transcriber_.store(std::move(transcriber));
  whisper_enabled_.store(true);
  whisper_gate_.active.store(true, std::memory_order_relaxed);
  return true;
}

std::optional<std::string> Stream::Implementation::WhisperModel() const {
  std::lock_guard lock{whisper_mutex_};
  if (whisper_model_path_.empty()) {
    return std::nullopt;
  }
  return whisper_model_path_;
}

bool Stream::Implementation::StartOutput(OutputMode output_mode,
                                         std::optional<int> connector_id) {
  std::lock_guard lock{mutex_};

  StopOutputPipeline(lock);

  output_mode_ = output_mode;
  connector_id_ = connector_id;

  ResetGuard reset{output_pipeline_,  output_source_, output_audio_source_,
                   preview_sink_,     preview_queue_, subtitle_overlay_,
                   subtitle_parser_,  subtitle_source_, output_bus_};

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
    InstallDropGate(preview_queue_.get(), preview_gate_);
  }

  if (subtitle_path_) {
    subtitle_overlay_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "subtitle_overlay")};
    subtitle_parser_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "subtitle_parser")};
    subtitle_source_ = ElementPtr{gst_bin_get_by_name(
        GST_BIN(output_pipeline_.get()), "subtitle_source")};

    if (!subtitle_overlay_ || !subtitle_parser_ || !subtitle_source_) {
      std::println(stderr, "Couldn't find subtitle branch elements");
      return false;
    }

    // Anchor the SRT timeline at the current running time: cue time t
    // renders at anchor + delay + t (#438). An output (re)start replays
    // the position and drops any pause (#439).
    subtitle_anchor_ = static_cast<gint64>(MasterRunningTime());
    subtitle_frozen_.reset();
    ApplySubtitleOffset();

    if (!subtitles_visible_) {
      g_object_set(subtitle_overlay_.get(), "silent", TRUE, nullptr);
    }

    // The renderer child is auto-plugged only once the pipeline runs,
    // so the stored style rides child-added (#159); the direct call
    // covers a renderer that already exists.
    g_signal_connect(subtitle_overlay_.get(), "child-added",
                     G_CALLBACK(&OnSubtitleChildAdded), this);
    ApplySubtitleStyle(SubtitleRenderer().get());
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
  // parser: textoverlay compares text and video by running time, so cue
  // time t lands on anchor + delay + t (#438).
  PadPtr pad{gst_element_get_static_pad(subtitle_parser_.get(), "src")};
  gst_pad_set_offset(pad.get(),
                     subtitle_anchor_ + subtitle_delay_.load());
}

void Stream::Implementation::ReparseSubtitles() {
  if (subtitle_source_ == nullptr) {
    return;
  }

  ApplySubtitleOffset();
  gst_element_seek_simple(subtitle_source_.get(), GST_FORMAT_BYTES,
                          GST_SEEK_FLAG_FLUSH, 0);
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
    // The session belongs to the previous file; a switch cancels it.
    CancelSyncLocked();
  }

  return StartOutput(output_mode, connector_id);
}

// Drops the session and resets the public state; call with mutex_
// held.
void Stream::Implementation::CancelSyncLocked() {
  std::lock_guard lock{sync_mutex_};
  sync_session_.reset();
  sync_state_ = {};
  sync_apply_pending_ = false;
}

Stream::SyncStartResult Stream::Implementation::StartSubtitleSync() {
  std::optional<std::string> path;
  {
    std::lock_guard lock{mutex_};
    path = subtitle_path_;
  }

  if (!path) {
    return SyncStartResult::kNoSubtitles;
  }

  if (!whisper_enabled_.load()) {
    return SyncStartResult::kNoWhisper;
  }

  std::ifstream file{*path};
  if (!file) {
    return SyncStartResult::kUnparseableSubtitles;
  }
  const std::string contents{std::istreambuf_iterator<char>{file},
                             std::istreambuf_iterator<char>{}};

  auto cues = ParseSrtCues(contents);
  if (cues.empty()) {
    return SyncStartResult::kUnparseableSubtitles;
  }

  // The deadline rides the shared timeline, so a capture clock that
  // stops also stops the session's idea of elapsed time.
  const auto deadline =
      static_cast<std::int64_t>(MasterRunningTime()) + kSyncListenWindowNs;

  {
    std::lock_guard lock{sync_mutex_};
    sync_session_.emplace(std::move(cues), MatchTranscript, deadline);
    sync_state_ = {};
    sync_state_.status = SyncStatus::kListening;
    sync_apply_pending_ = false;
  }

  return SyncStartResult::kStarted;
}

Stream::SyncState Stream::Implementation::SubtitleSync() const {
  std::lock_guard lock{sync_mutex_};
  return sync_state_;
}

void Stream::Implementation::SetSubtitleDelay(std::int64_t delay_ms) {
  std::lock_guard lock{mutex_};
  subtitle_delay_.store(delay_ms * static_cast<std::int64_t>(GST_MSECOND));
  // The pad offset reaches only cues parsed after the change: re-emit.
  // While paused the frozen position wins; the trim applies on resume.
  if (!subtitle_frozen_) {
    ReparseSubtitles();
  }
}

std::optional<std::int64_t> Stream::Implementation::SubtitleTime() {
  std::lock_guard lock{mutex_};

  if (subtitle_parser_ == nullptr) {
    return std::nullopt;
  }

  const gint64 position =
      subtitle_frozen_.value_or(static_cast<gint64>(MasterRunningTime()) -
                                subtitle_anchor_ - subtitle_delay_.load());
  return position / static_cast<gint64>(GST_MSECOND);
}

void Stream::Implementation::SetSubtitleTime(std::int64_t time_ms) {
  std::lock_guard lock{mutex_};

  // A manual seek cancels the session: last action wins.
  CancelSyncLocked();
  ApplySubtitleTimeLocked(time_ms);
}

// Moves the SRT position; call with mutex_ held.
void Stream::Implementation::ApplySubtitleTimeLocked(std::int64_t time_ms) {
  if (subtitle_parser_ == nullptr) {
    return;
  }

  const gint64 position = time_ms * static_cast<gint64>(GST_MSECOND);

  // Hidden while paused; the branch re-parses on resume.
  if (subtitle_frozen_) {
    subtitle_frozen_ = position;
    return;
  }

  subtitle_anchor_ = static_cast<gint64>(MasterRunningTime()) -
                     subtitle_delay_.load() - position;
  ReparseSubtitles();
}

void Stream::Implementation::SetSubtitlesPaused(bool paused) {
  std::lock_guard lock{mutex_};

  if (subtitle_parser_ == nullptr ||
      paused == subtitle_frozen_.has_value()) {
    return;
  }

  if (paused) {
    subtitle_frozen_ = static_cast<gint64>(MasterRunningTime()) -
                       subtitle_anchor_ - subtitle_delay_.load();
  } else {
    // Resume from the frozen position: re-anchor and re-emit the SRT.
    subtitle_anchor_ = static_cast<gint64>(MasterRunningTime()) -
                       subtitle_delay_.load() - *subtitle_frozen_;
    subtitle_frozen_.reset();
    ReparseSubtitles();
  }

  if (subtitle_overlay_ != nullptr) {
    g_object_set(subtitle_overlay_.get(), "silent", paused, nullptr);
  }
}

bool Stream::Implementation::SubtitlesPaused() {
  std::lock_guard lock{mutex_};
  return subtitle_frozen_.has_value();
}

void Stream::Implementation::SetSubtitlesVisible(bool visible) {
  std::lock_guard lock{mutex_};
  subtitles_visible_ = visible;
  if (subtitle_overlay_ != nullptr) {
    g_object_set(subtitle_overlay_.get(), "silent", !visible, nullptr);
  }
}

bool Stream::Implementation::SubtitlesVisible() {
  std::lock_guard lock{mutex_};
  return subtitles_visible_;
}

std::int64_t Stream::Implementation::SubtitleDelay() {
  return subtitle_delay_.load() / static_cast<std::int64_t>(GST_MSECOND);
}

ElementPtr Stream::Implementation::SubtitleRenderer() const {
  if (subtitle_overlay_ == nullptr) {
    return nullptr;
  }
  // Transfer-full; the GObject cast adds no reference.
  return ElementPtr{GST_ELEMENT(gst_child_proxy_get_child_by_name(
      GST_CHILD_PROXY(subtitle_overlay_.get()), "renderer"))};
}

void Stream::Implementation::ApplySubtitleStyle(GstElement* renderer) const {
  const auto style = subtitle_style_.load();
  if (renderer == nullptr || style == nullptr) {
    return;
  }

  if (style->family.has_value() || style->size_pt.has_value()) {
    std::string desc = style->family.value_or("");
    if (style->size_pt.has_value()) {
      if (!desc.empty()) {
        desc += ' ';
      }
      desc += std::to_string(*style->size_pt);
    }
    g_object_set(renderer, "font-desc", desc.c_str(), nullptr);
  }

  if (style->color_argb.has_value()) {
    g_object_set(renderer, "color", *style->color_argb, nullptr);
  }
}

/* static */
void Stream::Implementation::OnSubtitleChildAdded(GstChildProxy*,
                                                  GObject* child, gchar* name,
                                                  gpointer user_data) {
  if (std::string_view{name} == "renderer") {
    static_cast<Implementation*>(user_data)->ApplySubtitleStyle(
        GST_ELEMENT(child));
  }
}

void Stream::Implementation::SetSubtitleFontFamily(std::string family) {
  std::lock_guard lock{mutex_};

  auto style = std::make_shared<SubtitleStyle>();
  if (const auto current = subtitle_style_.load(); current != nullptr) {
    *style = *current;
  }
  style->family = std::move(family);
  subtitle_style_ = std::move(style);

  ApplySubtitleStyle(SubtitleRenderer().get());
}

void Stream::Implementation::SetSubtitleFontSize(std::int64_t size_pt) {
  std::lock_guard lock{mutex_};

  auto style = std::make_shared<SubtitleStyle>();
  if (const auto current = subtitle_style_.load(); current != nullptr) {
    *style = *current;
  }
  style->size_pt = size_pt;
  subtitle_style_ = std::move(style);

  ApplySubtitleStyle(SubtitleRenderer().get());
}

void Stream::Implementation::SetSubtitleFontColor(std::uint32_t color_argb) {
  std::lock_guard lock{mutex_};

  auto style = std::make_shared<SubtitleStyle>();
  if (const auto current = subtitle_style_.load(); current != nullptr) {
    *style = *current;
  }
  style->color_argb = color_argb;
  subtitle_style_ = std::move(style);

  ApplySubtitleStyle(SubtitleRenderer().get());
}

std::optional<std::string> Stream::Implementation::SubtitleFontFamily() const {
  if (const auto style = subtitle_style_.load(); style != nullptr) {
    return style->family;
  }
  return std::nullopt;
}

std::optional<std::int64_t> Stream::Implementation::SubtitleFontSize() const {
  if (const auto style = subtitle_style_.load(); style != nullptr) {
    return style->size_pt;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> Stream::Implementation::SubtitleFontColor()
    const {
  if (const auto style = subtitle_style_.load(); style != nullptr) {
    return style->color_argb;
  }
  return std::nullopt;
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
    whisper_thread_.request_stop();

    // Changing state unblocks pending appsink operations.
    if (capture_pipeline_ != nullptr) {
      gst_element_set_state(capture_pipeline_.get(), GST_STATE_NULL);
    }

    capture_thread_.join();

    if (audio_thread_.joinable()) {
      audio_thread_.join();
    }

    // Joining can take a full window's inference time; whisper_full has
    // no cancellation point.
    if (whisper_thread_.joinable()) {
      whisper_thread_.join();
    }

    // No partial capture data may survive into the no-signal screen (or
    // a restarted capture): drop whatever the frame buffers still hold.
    frames_.Flush();
    audio_.Flush();

    ResetGuard reset{capture_pipeline_, capture_sink_,  capture_audio_sink_,
                     whisper_sink_,     whisper_queue_, capture_bus_};
  }

  // The session listens to capture audio; capture going away ends it.
  {
    std::lock_guard sync_lock{sync_mutex_};
    if (sync_session_) {
      sync_state_.status = SyncStatus::kFailed;
      sync_state_.reason = "capture stopped";
      sync_session_.reset();
    }
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

    ResetGuard reset{output_pipeline_,  output_source_, output_audio_source_,
                     preview_sink_,     preview_queue_, subtitle_overlay_,
                     subtitle_parser_,  subtitle_source_, output_bus_};
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

  {
    std::lock_guard sync_lock{sync_mutex_};

    // Apply a lock's position here: the whisper thread flagged it
    // because it must not take mutex_.
    if (sync_apply_pending_) {
      sync_apply_pending_ = false;
      if (sync_state_.time_ms) {
        ApplySubtitleTimeLocked(*sync_state_.time_ms);
      }
    }

    // Sessions starved of windows (whisper toggled off, capture lost)
    // still meet their deadline.
    if (sync_session_) {
      const auto result =
          sync_session_->Poll(static_cast<std::int64_t>(MasterRunningTime()));
      if (result.state == SyncSession::State::kFailed) {
        STREAM_LOG(LogLevel::kInfo, "Subtitle sync failed: {}", result.reason);
        sync_state_.status = SyncStatus::kFailed;
        sync_state_.reason = result.reason;
        sync_session_.reset();
      }
    }
  }
}

bool Stream::Implementation::Initialize(
    const std::string& device, OutputMode output_mode,
    std::optional<int> connector_id, bool audio,
    const std::optional<std::string>& audio_output_device,
    std::int64_t audio_offset_ms, bool preview,
    const std::optional<std::string>& subtitles,
    const std::optional<std::string>& whisper_model) {
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

  // The flag is an explicit enable: a model that won't load is fatal,
  // like a missing --subtitles file.
  if (whisper_model && !SetWhisperState(true, *whisper_model)) {
    std::println(stderr, "Couldn't load the whisper model: {}", *whisper_model);
    return false;
  }

  const std::optional<std::string_view> branch_device =
      audio_enabled_
          ? std::make_optional<std::string_view>(audio_output_device_)
          : std::nullopt;

  STREAM_LOG(LogLevel::kDebug, "Capture pipeline:\n{}\n\nOutput pipeline:\n{}",
             CapturePipelineDescription(device, audio_enabled_,
                                        audio_enabled_ && kWhisperAvailable),
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
    const std::optional<std::string>& subtitles,
    const std::optional<std::string>& whisper_model) {
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
                                  subtitles, whisper_model)) {
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

bool Stream::SetWhisperState(bool enabled,
                             const std::optional<std::string>& model_path) {
  return implementation_->SetWhisperState(enabled, model_path);
}

bool Stream::WhisperEnabled() const {
  return implementation_->WhisperEnabled();
}

std::optional<std::string> Stream::WhisperModel() const {
  return implementation_->WhisperModel();
}

Stream::SyncStartResult Stream::StartSubtitleSync() {
  return implementation_->StartSubtitleSync();
}

Stream::SyncState Stream::SubtitleSync() const {
  return implementation_->SubtitleSync();
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

std::optional<std::int64_t> Stream::SubtitleTime() const {
  return implementation_->SubtitleTime();
}

void Stream::SetSubtitleTime(std::int64_t time_ms) {
  implementation_->SetSubtitleTime(time_ms);
}

void Stream::SetSubtitlesPaused(bool paused) {
  implementation_->SetSubtitlesPaused(paused);
}

bool Stream::SubtitlesPaused() const {
  return implementation_->SubtitlesPaused();
}

bool Stream::SubtitlesVisible() const {
  return implementation_->SubtitlesVisible();
}

std::int64_t Stream::SubtitleDelay() const {
  return implementation_->SubtitleDelay();
}

void Stream::SetSubtitleFontFamily(std::string family) {
  implementation_->SetSubtitleFontFamily(std::move(family));
}

void Stream::SetSubtitleFontSize(std::int64_t size_pt) {
  implementation_->SetSubtitleFontSize(size_pt);
}

void Stream::SetSubtitleFontColor(std::uint32_t color_argb) {
  implementation_->SetSubtitleFontColor(color_argb);
}

std::optional<std::string> Stream::SubtitleFontFamily() const {
  return implementation_->SubtitleFontFamily();
}

std::optional<std::int64_t> Stream::SubtitleFontSize() const {
  return implementation_->SubtitleFontSize();
}

std::optional<std::uint32_t> Stream::SubtitleFontColor() const {
  return implementation_->SubtitleFontColor();
}

bool Stream::Failed() const { return implementation_->Failed(); }

std::uint64_t Stream::DroppedFrames() const {
  return implementation_->DroppedFrames();
}

}  // namespace subtitler
