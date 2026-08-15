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

std::string capture_pipeline_description(std::string_view device);

std::string output_pipeline_description(OutputMode mode,
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
  std::uint64_t dropped_frames() const;

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace subtitler
