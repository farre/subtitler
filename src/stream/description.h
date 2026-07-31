#pragma once

#include <optional>
#include <string_view>

namespace subtitler {
std::string capture_pipeline_description(std::string_view device);

std::string output_pipeline_description(std::optional<int> connector_id);
}  // namespace subtitler
