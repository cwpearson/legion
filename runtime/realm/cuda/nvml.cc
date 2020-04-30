#include "realm/cuda/nvml.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

#define NVML(stmt) checkNvml(stmt, __FILE__, __LINE__);

namespace Realm {
namespace nvml {

// TODO: integrate with Legion error handling
void checkNvml(nvmlReturn_t result, const char *file, const int line) {
  if (NVML_SUCCESS != result) {
    fprintf(stderr, "nvml Error: %s in %s : %d\n", nvmlErrorString(result),
            file, line);
    exit(-1);
  }
}

static bool once = false;
void lazy_init() {
  if (!once) {
    std::cerr << "nvmlInit()\n";
    NVML(nvmlInit());
    once = true;
  }
}

/*static*/ const Distance Distance::UNKNOWN_DISTANCE = {};

Distance get_gpu_gpu_distance(int src, int dst) {
  std::cerr << src << " " << dst << "\n";

  Distance distance = Distance::UNKNOWN_DISTANCE;

  if (src == dst) {
    distance.kind = DistanceKind::SAME;
    return distance;
  }

  // get NVML device handles
  nvmlDevice_t srcDev, dstDev;
  NVML(nvmlDeviceGetHandleByIndex(src, &srcDev));
  NVML(nvmlDeviceGetHandleByIndex(dst, &dstDev));

  // zero width and version.
  // if NvLink, width will be incremented as links are found
  distance.width = 0;
  distance.version = 0;

  for (int link = 0; link < 6; ++link) {
    nvmlPciInfo_t pci;
    nvmlReturn_t ret = nvmlDeviceGetNvLinkRemotePciInfo(srcDev, link, &pci);
    if (NVML_SUCCESS == ret) {
      // use remote PCI to see if remote device is dst
      nvmlDevice_t remote;
      nvmlReturn_t getRet = nvmlDeviceGetHandleByPciBusId(pci.busId, &remote);
      if (NVML_SUCCESS == getRet) {
        if (remote == dstDev) {
          distance.kind = DistanceKind::NVLINK_CLOSE;
          ++distance.width;

          {
            unsigned int version;
            nvmlReturn_t ret =
                nvmlDeviceGetNvLinkVersion(srcDev, link, &version);
            if (NVML_SUCCESS == ret) {
              distance.version = version;
            } else {
              NVML(ret);
            }
          }

          return distance;
        }
      } else if (NVML_ERROR_NOT_FOUND == getRet) {
        // not attached to a GPU

        // only set to far if we didn't already discover that the devices are
        // close
        if (distance.kind != DistanceKind::NVLINK_CLOSE) {
          distance.kind = DistanceKind::NVLINK_FAR;
        }
        continue; // try next link
      } else {
        NVML(getRet);
      }

    } else if (NVML_ERROR_INVALID_ARGUMENT == ret) {
      // device is invalid
      // link is invalid
      // pci is null
      continue;
    } else if (NVML_ERROR_NOT_SUPPORTED == ret) {
      // device does not support
      break;
    } else {
      NVML(ret);
    }
  }

  // no nvlink, try other methods
  nvmlGpuTopologyLevel_t pathInfo;
  NVML(nvmlDeviceGetTopologyCommonAncestor(srcDev, dstDev, &pathInfo));
  switch (pathInfo) {
  case NVML_TOPOLOGY_INTERNAL:
    distance.kind = DistanceKind::SAME;
    break;
  case NVML_TOPOLOGY_SINGLE:
    distance.kind = DistanceKind::PCIE_CLOSE;
    break;
  case NVML_TOPOLOGY_MULTIPLE:
    distance.kind = DistanceKind::PCIE_CLOSE;
    break;
  case NVML_TOPOLOGY_HOSTBRIDGE:
    distance.kind = DistanceKind::PCIE_CLOSE;
    break;
  case NVML_TOPOLOGY_NODE:
    distance.kind = DistanceKind::PCIE_FAR;
    break;
  case NVML_TOPOLOGY_SYSTEM:
    distance.kind = DistanceKind::PCIE_FAR;
    break;
  }

  return distance;
}

std::ostream &operator<<(std::ostream &os, const Distance &d) {
  switch (d.kind) {
  case DistanceKind::SAME: {
    os << "same";
    return os;
  }
  case DistanceKind::PCIE_CLOSE: {
    os << "pcie/close";
    return os;
  }
  case DistanceKind::PCIE_FAR: {
    os << "pcie/far";
    return os;
  }
  case DistanceKind::NVLINK_CLOSE: {
    os << "nvlink/close/v" << d.version << "/w" << d.width;
    return os;
  }
  case DistanceKind::NVLINK_FAR: {
    os << "nvlink/far/v" << d.version << "/w" << d.width;
    return os;
  }
  case DistanceKind::UNKNOWN: {
    os << "unknown";
    return os;
  }
  }
}

} // namespace nvml
} // namespace Realm