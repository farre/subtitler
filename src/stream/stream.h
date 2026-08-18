#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
// the audio branch playing through that ALSA device.
std::string OutputPipelineDescription(
    OutputMode mode, std::optional<int> connector_id,
    std::optional<std::string_view> audio_device);
std::string VideoOutputPipelineDescription(OutputMode mode,
                                           std::optional<int> connector_id);
std::string AudioOutputPipelineDescription(std::string_view device);

class Stream {
  struct Implementation;

 public:
  // A null audio_output_device auto-detects the connected vc4-hdmi port.
  static std::unique_ptr<Stream> Create(
      const std::string& device, OutputMode output_mode,
      std::optional<int> connector_id, bool audio,
      const std::optional<std::string>& audio_output_device);
  ~Stream();

  void Poll();
  void Stop();

  bool RestartCapture(const std::string& device);
  bool RestartOutput(OutputMode output_mode, std::optional<int> connector_id);

  bool Failed() const;
  std::uint64_t DroppedFrames() const;

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
