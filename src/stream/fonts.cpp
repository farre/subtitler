#include "stream/fonts.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <string>

namespace subtitler {

std::vector<std::string> AvailableFontFamilies() {
  PangoFontFamily** families = nullptr;
  int count = 0;
  pango_font_map_list_families(pango_cairo_font_map_get_default(), &families,
                               &count);

  std::vector<std::string> names;
  names.reserve(count > 0 ? static_cast<std::size_t>(count) : 0);
  for (int i = 0; i < count; ++i) {
    names.emplace_back(pango_font_family_get_name(families[i]));
  }
  g_free(families);

  std::ranges::sort(names);
  return names;
}

}  // namespace subtitler
