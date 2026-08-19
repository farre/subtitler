#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "utils/preview_frame.h"

namespace subtitler {

enum class OutputMode {
  kKmsPisp,
  kKmsSoftware,
  kWindow,
  kNull,
};

// The full capture pipeline: video branch plus, when audio is true, the
// audio branch. The branches are separate functions so machines without
// the capture device's ALSA card can run video-only.
std::string CapturePipelineDescription(std::string_view device, bool audio);
std::string VideoCapturePipelineDescription(std::string_view device);
std::string AudioCapturePipelineDescription(std::string_view device);

// The full output pipeline: video branch plus, when audio_device is set,
// the audio branch playing through that ALSA device. When preview is true
// the video branch tees off a gated JPEG preview branch for the web
// interface. When subtitles is set, a subtitleoverlay composites the SRT
// at that path onto the video before the tee (#438).
std::string OutputPipelineDescription(
    OutputMode mode, std::optional<int> connector_id,
    std::optional<std::string_view> audio_device, bool preview = false,
    std::optional<std::string_view> subtitles = std::nullopt);
std::string VideoOutputPipelineDescription(
    OutputMode mode, std::optional<int> connector_id, bool preview = false,
    std::optional<std::string_view> subtitles = std::nullopt);
std::string AudioOutputPipelineDescription(std::string_view device);

class Stream {
  struct Implementation;

 public:
  // A null audio_output_device auto-detects the connected vc4-hdmi port.
  // audio_offset_ms shifts audio relative to video: positive delays audio,
  // negative advances it (realized by delaying video; latency grows by
  // |offset|). When subtitles is set, cues from the SRT at that path are
  // composited onto the video, anchored at the running time the output
  // starts (#438).
  static std::unique_ptr<Stream> Create(
      const std::string& device, OutputMode output_mode,
      std::optional<int> connector_id, bool audio,
      const std::optional<std::string>& audio_output_device,
      std::int64_t audio_offset_ms = 0, bool preview = false,
      const std::optional<std::string>& subtitles = std::nullopt);
  ~Stream();

  void Poll();
  void Stop();

  bool RestartCapture(const std::string& device);
  bool RestartOutput(OutputMode output_mode, std::optional<int> connector_id);

  // Switches the rendered SRT (or detaches subtitles entirely for
  // nullopt) on a live stream. Rebuilds the output side through the
  // restart-safe path — capture is never restarted — and re-anchors the
  // new file at the current running time with the delay reset to zero.
  bool SetSubtitleFile(const std::optional<std::string>& path);

  // Live subtitle delay trim in milliseconds; positive delays cues.
  // Applies without rebuilding the pipeline (#169).
  void SetSubtitleDelay(std::int64_t delay_ms);

  // Live show/hide toggle; does not disturb the subtitle branch (#158).
  void SetSubtitlesVisible(bool visible);

  // The latest encoded preview frame, fed while the preview branch is
  // active. The web server reads from this buffer.
  PreviewFrameBuffer& PreviewFrames();

  // Opens (true) or closes (false) the preview branch's gate. Closed is
  // the default, so no JPEG encoding happens without web clients.
  void SetPreviewActive(bool active);

  bool Failed() const;
  std::uint64_t DroppedFrames() const;

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
