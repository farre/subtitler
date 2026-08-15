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

std::string CapturePipelineDescription(std::string_view device);

std::string OutputPipelineDescription(OutputMode mode,
                                      std::optional<int> connector_id);

class Stream {
  struct Implementation;

 public:
  static std::unique_ptr<Stream> Create(const std::string& device,
                                        OutputMode output_mode,
                                        std::optional<int> connector_id);
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
