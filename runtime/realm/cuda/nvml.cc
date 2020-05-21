#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>

#include <nvml.h>

#include "realm/cuda/nvml.h"

#include "realm/logging.h"

#define NVML(stmt) checkNvml(stmt, __FILE__, __LINE__);

namespace Realm {
namespace nvml {

Logger log_nvml("nvml");

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

void add_gpus(system::System &sys) {

  using system::Node;
  using system::NodeType;

  lazy_init();

  unsigned int count;
  NVML(nvmlDeviceGetCount(&count));

  for (unsigned di = 0; di < count; ++di) {

    Node *gpu = sys.get_gpu(di);
    if (!gpu) {
      gpu = new Node;
      gpu->type = NodeType::gpu;
      gpu->id = di;
      sys.add_node(gpu);
      assert(sys.get_gpu(di));
      log_nvml.debug() << " added GPU " << di;
    }
  }
}

void add_nvlinks(system::System &sys, int srcId) {

  using system::Link;
  using system::Node;

  lazy_init();

  system::Node *gpu = sys.get_gpu(srcId);
  if (!gpu) {
    log_nvml.error("looking for GPU %d but wasn't found in system", srcId);
  }
  assert(gpu);

  nvmlDevice_t dev;
  NVML(nvmlDeviceGetHandleByIndex(srcId, &dev));

  /* Check all NvLinks. If the GPUs are directly connected, then count the
  number of links If the src GPU has an NvLink but is not directly connected to
  the dst, then assume there is a faster-than-PCIe path (NVLINK_FAR),
  */
  for (int li = 0; li < 6; ++li) {
    system::Link link;

    nvmlPciInfo_t pci;
    nvmlReturn_t ret = nvmlDeviceGetNvLinkRemotePciInfo(dev, li, &pci);
    if (NVML_SUCCESS == ret) {
      nvmlDevice_t remote;
      nvmlReturn_t getRet = nvmlDeviceGetHandleByPciBusId(pci.busId, &remote);

      if (NVML_SUCCESS == getRet) { // remote is a GPU

        // the remote is an NVML device, should have already been added to the
        // system
        unsigned int remoteId;
        NVML(nvmlDeviceGetIndex(remote, &remoteId));
        system::Node *remoteGpu = sys.get_gpu(remoteId);
        assert(remoteGpu);

        // if NvLink already exists, add 1 to width
        // otherwise, create nvlink of width 1
        // we only add for gpu < remoteGpu, so we dont double-count links
        if (gpu < remoteGpu) {
          system::Link *link = sys.get_link(gpu, remoteGpu);
          if (link) {
            assert(link->type == system::LinkType::nvlink);
            assert(link->nvlink.version > 0);
            link->nvlink.width += 1;
          } else {
            link = new system::Link;
            link->type = system::LinkType::nvlink;
            link->nvlink.width = 1;
            unsigned int version;
            NVML(nvmlDeviceGetNvLinkVersion(dev, li, &version));
            link->nvlink.version = version;
            sys.add_link(gpu, remoteGpu, link);
          }
        }
      } else if (NVML_ERROR_NOT_FOUND ==
                 getRet) { // nvmlDeviceGetNvLinkRemotePciInfo
        // the remote is not a GPU

        const unsigned short pciVendorId = (pci.pciDeviceId >> 16) & 0xFF;
        const unsigned short pciDeviceId = pci.pciDeviceId & 0xFF;

        // Iremote is an IBM emulated NvLink Bridge
        if (0x1041 == pciVendorId && 0x04ea == pciDeviceId) {

          log_nvml.info(
              "GPU %d link %d is an IBM emulated NvLink Bridge (04ea)", srcId,
              li);

          // get the CPU affinity
          system::CpuSet cpuset;
          NVML(nvmlDeviceGetCpuAffinity(dev, system::CpuSetSize, cpuset));

          // get the sockets we have affinity for
          std::set<system::Node *> sockets;
          for (size_t f = 0; f < system::CpuSetSize; ++f) {
            for (size_t b = 0; b < sizeof(cpuset[0]) * 8; ++b) {
              if ((cpuset[f] >> b) & 0x1) {
                system::Node *socket =
                    sys.get_socket_for_cpu(f * sizeof(cpuset[0]) * 8 + b);
                assert(socket);
                sockets.insert(socket);
              }
            }
          }
          assert(sockets.size() == 1);
          system::Node *cpu = *sockets.begin();

          system::Link *link = sys.get_link(gpu, cpu);
          if (!link) {
            link = new system::Link;
            link->type = system::LinkType::nvlink;
            link->nvlink.width = 1;

            unsigned int version;
            NVML(nvmlDeviceGetNvLinkVersion(dev, li, &version));
            link->nvlink.version = version;
            sys.add_link(gpu, cpu, link);
          } else {
            link->nvlink.width += 1;
          }
        } else { // not an IBM emulated bridge
          assert(false && "unexpected remote nvlink device");
        }
      } else { // nvmlDeviceGetNvLinkRemotePciInfo
        NVML(getRet);
      }

    } else if (NVML_ERROR_INVALID_ARGUMENT == ret) {
      // device is invalid
      // link is invalid
      // pci is null
      continue;
    } else if (NVML_ERROR_NOT_SUPPORTED == ret) {
      // device does not support
      log_nvml.debug("GPU %d does not support nvlink", srcId);
      break;
    } else {
      NVML(ret);
    }
  }

  log_nvml.debug("finished add_nvlinks(..., %d)" , srcId);
}

void add_nvlinks(system::System &sys) {
  lazy_init();
  unsigned int count;
  NVML(nvmlDeviceGetCount(&count));
  for (unsigned di = 0; di < count; ++di) {
    add_nvlinks(sys, di);
  }
}

} // namespace nvml
} // namespace Realm
