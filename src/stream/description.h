#pragma once

#include <optional>
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
}  // namespace subtitler
