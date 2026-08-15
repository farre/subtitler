#include "probe/drm.h"

#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <memory>
#include <string>

#include "probe/fd.h"

namespace {

template <typename T, void (*Free)(T*)>
struct DrmDeleter {
  void operator()(T* object) const noexcept {
    if (object != nullptr) {
      Free(object);
    }
  }
};

template <typename T, void (*Free)(T*)>
using DrmPointer = std::unique_ptr<T, DrmDeleter<T, Free>>;

using VersionPtr = DrmPointer<drmVersion, drmFreeVersion>;
using ResPtr = DrmPointer<drmModeRes, drmModeFreeResources>;
using ConnectorPtr = DrmPointer<drmModeConnector, drmModeFreeConnector>;
using PlaneResPtr = DrmPointer<drmModePlaneRes, drmModeFreePlaneResources>;
using PlanePtr = DrmPointer<drmModePlane, drmModeFreePlane>;

std::string FourccToString(std::uint32_t fourcc) {
  std::string result(4, '\0');
  for (int i = 0; i < 4; ++i) {
    result[i] = static_cast<char>((fourcc >> (8 * i)) & 0xff);
  }
  return result;
}

subtitler::probe::Fd OpenCard(const std::string& driver_name) {
  for (int i = 0; i < 16; ++i) {
    const auto path = "/dev/dri/card" + std::to_string(i);
    subtitler::probe::Fd fd{open(path.c_str(), O_RDWR | O_CLOEXEC)};
    if (!fd) {
      continue;
    }

    const VersionPtr version{drmGetVersion(fd.get())};
    if (version != nullptr && driver_name == version->name) {
      return fd;
    }
  }
  return subtitler::probe::Fd{};
}

}  // namespace

namespace subtitler::probe {

DrmInfo ProbeDrm(const std::string& driver_name) {
  const Fd fd = OpenCard(driver_name);
  if (!fd) {
    return {};
  }

  DrmInfo info{.driver = driver_name, .connectors = {}, .planes = {}};

  const ResPtr resources{drmModeGetResources(fd.get())};
  if (resources != nullptr) {
    for (int i = 0; i < resources->count_connectors; ++i) {
      const ConnectorPtr connector{
          drmModeGetConnector(fd.get(), resources->connectors[i])};
      if (connector == nullptr) {
        continue;
      }
      info.connectors.push_back(DrmConnector{
          .id = static_cast<int>(connector->connector_id),
          .name = std::string{drmModeGetConnectorTypeName(
                      connector->connector_type)} +
                  std::to_string(connector->connector_type_id),
          .connected = connector->connection == DRM_MODE_CONNECTED,
      });
    }
  }

  // Overlays and cursors are hidden without this.
  drmSetClientCap(fd.get(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  const PlaneResPtr plane_resources{drmModeGetPlaneResources(fd.get())};
  if (plane_resources != nullptr) {
    for (std::uint32_t i = 0; i < plane_resources->count_planes; ++i) {
      const PlanePtr plane{
          drmModeGetPlane(fd.get(), plane_resources->planes[i])};
      if (plane == nullptr) {
        continue;
      }
      DrmPlane entry{.id = static_cast<int>(plane->plane_id), .formats = {}};
      for (std::uint32_t f = 0; f < plane->count_formats; ++f) {
        entry.formats.push_back(FourccToString(plane->formats[f]));
      }
      info.planes.push_back(std::move(entry));
    }
  }

  return info;
}

}  // namespace subtitler::probe
