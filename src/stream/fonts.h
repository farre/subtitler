#pragma once

#include <string>
#include <vector>

namespace subtitler {

// The font families the subtitle renderer can use (#159), enumerated
// from pango's font map. Empty when no fonts are installed.
std::vector<std::string> AvailableFontFamilies();

}  // namespace subtitler
