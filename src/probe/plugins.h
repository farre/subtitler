#pragma once

#include <string>
#include <vector>

namespace subtitler::probe {

struct ElementAvailability {
  std::string name;
  bool available;
};

std::vector<ElementAvailability> probe_elements();

}  // namespace subtitler::probe
