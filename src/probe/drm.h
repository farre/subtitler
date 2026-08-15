#pragma once

#include <string>
#include <vector>

namespace subtitler::probe {

struct DrmConnector {
  int id;
  std::string name;
  bool connected;
};

struct DrmPlane {
  int id;
  std::vector<std::string> formats;  // fourcc strings, e.g. "NV12"
};

struct DrmInfo {
  std::string driver;
  std::vector<DrmConnector> connectors;
  std::vector<DrmPlane> planes;
};

// Empty when no card for the driver exists.
DrmInfo ProbeDrm(const std::string& driver_name);

}  // namespace subtitler::probe
