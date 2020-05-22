#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <set>

#include <nvml.h>

#include "realm/cuda/nvml.h"

#include "realm/logging.h"

#define NVML(stmt) checkNvml(stmt, __FILE__, __LINE__);
#define NVML_FATAL(stmt) checkNvml(stmt, __FILE__, __LINE__, true);

namespace Realm {
namespace nvml {

Logger log("nvml");

void checkNvml(nvmlReturn_t result, const char *file, const int line,
               bool fatal = false) {
  if (NVML_SUCCESS != result) {
    if (fatal) {
      log.fatal("nvml Error: %s in %s : %d\n", nvmlErrorString(result), file,
                line);
    } else {
      log.error("nvml Error: %s in %s : %d\n", nvmlErrorString(result), file,
                line);
    }
  }
}

static bool once = false;
void lazy_init() {
  if (!once) {
    log.info("call nvmlInit()");
    NVML_FATAL(nvmlInit());
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
      log.debug() << " added GPU " << di;
    }
  }
}

void add_nvlinks(system::System &sys, int srcId) {

  using system::Link;
  using system::Node;

  lazy_init();

  system::Node *gpu = sys.get_gpu(srcId);
  if (!gpu) {
    log.error("looking for GPU %d but wasn't found in system", srcId);
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

        log.info("GPU %d link %d is an NVML device", srcId, li);
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
            log.info("nvlink gpus %d-%d, width=%d", gpu->id, remoteGpu->id,
                     link->nvlink.width);
          } else {
            link = new system::Link;
            link->type = system::LinkType::nvlink;
            link->nvlink.width = 1;
            unsigned int version;
            NVML(nvmlDeviceGetNvLinkVersion(dev, li, &version));
            link->nvlink.version = version;
            log.info("added nvlink gpus %d-%d, width=%d version=%d", gpu->id,
                     remoteGpu->id, link->nvlink.width, link->nvlink.version);
            sys.add_link(gpu, remoteGpu, link);
          }
        }
      } else if (NVML_ERROR_NOT_FOUND ==
                 getRet) { // nvmlDeviceGetNvLinkRemotePciInfo
        // the remote is not a GPU

        const unsigned short pciVendorId = (pci.pciDeviceId >> 16) & 0xFFFF;
        const unsigned short pciDeviceId = pci.pciDeviceId & 0xFFFF;
        // TODO: I think the vendor ID should be 0x1041, but NVML reports 0, so
        // ignore for now Iremote is an IBM emulated NvLink Bridge
        if (0x04ea == pciDeviceId) {
          log.info("GPU %d link %d is an IBM emulated NvLink Bridge (04ea)",
                   srcId, li);

          // get the CPU affinity
          system::CpuSet cpuset;
          NVML(nvmlDeviceGetCpuAffinity(dev, system::CpuSetSize, cpuset));

          // get the sockets we have affinity for
          std::vector<system::Node *> sockets =
              sys.get_sockets_for_cpuset(cpuset);
          assert(sockets.size() == 1);
          system::Node *cpu = *sockets.begin();

          system::Link *link = sys.get_link(gpu, cpu);
          if (!link) {
            link = new system::Link();
            link->type = system::LinkType::nvlink;
            link->nvlink.width = 1;

            unsigned int version;
            NVML(nvmlDeviceGetNvLinkVersion(dev, li, &version));
            link->nvlink.version = version;
            sys.add_link(gpu, cpu, link);
          } else {
            link->nvlink.width += 1;
            log.error("link bw gpu %d and socket %d width=%d", gpu->id, cpu->id,
                      link->nvlink.width);
          }
        } else { // not an IBM emulated bridge
          log.error() << "unexpected remote nvlink device vendor=" << std::hex
                      << (pci.pciDeviceId >> 16)
                      << " device=" << (pci.pciDeviceId & 0xFFFF);
          assert(false);
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
      log.debug("GPU %d does not support nvlink", srcId);
      break;
    } else {
      NVML(ret);
    }
  }

  log.debug("finished add_nvlinks(..., %d)", srcId);
}

void add_nvlinks(system::System &sys) {
  lazy_init();
  unsigned int count;
  NVML(nvmlDeviceGetCount(&count));
  for (unsigned di = 0; di < count; ++di) {
    add_nvlinks(sys, di);
  }
}

void add_pci(system::System &sys) {
  lazy_init();

  using namespace system;

  unsigned int count;
  NVML(nvmlDeviceGetCount(&count));

  // add gpu-gpu links
  for (unsigned src = 0; src < count; ++src) {
    for (unsigned dst = 0; dst < count; ++dst) {

      if (src != dst) {
        Node *srcGpu = sys.get_gpu(src);
        Node *dstGpu = sys.get_gpu(dst);
	if (!srcGpu) {
          log.error("didn't find GPU %d in system", src);
	  continue;
	}
	if (!srcGpu) {
          log.error("didn't find GPU %d in system", dst);
	  continue;
	}


        std::vector<system::System::Path> paths = sys.paths(srcGpu, dstGpu);

        // if no path between the GPUs, fall back to PCI distance
        if (paths.empty()) {

          nvmlDevice_t srcDev, dstDev;
          NVML(nvmlDeviceGetHandleByIndex(src, &srcDev));
          NVML(nvmlDeviceGetHandleByIndex(dst, &dstDev));

          Link *link = new Link();
          link->type = LinkType::pci;
          link->pci.ancestor = PciAncestor::unknown;

          nvmlGpuTopologyLevel_t pathInfo;
          NVML(nvmlDeviceGetTopologyCommonAncestor(srcDev, dstDev, &pathInfo));
          switch (pathInfo) {
          case NVML_TOPOLOGY_INTERNAL:
            link->pci.ancestor = PciAncestor::internal;
            break;
          case NVML_TOPOLOGY_SINGLE:
            link->pci.ancestor = PciAncestor::single;
            break;
          case NVML_TOPOLOGY_MULTIPLE:
            link->pci.ancestor = PciAncestor::multiple;
            break;
          case NVML_TOPOLOGY_HOSTBRIDGE:
            link->pci.ancestor = PciAncestor::hostbridge;
            break;
          case NVML_TOPOLOGY_NODE:
            link->pci.ancestor = PciAncestor::node;
            break;
          case NVML_TOPOLOGY_SYSTEM:
            link->pci.ancestor = PciAncestor::system;
            break;
          }

          log.info("add pci link gpu %d gpu %d", srcGpu->id, dstGpu->id);
          sys.add_link(srcGpu, dstGpu, link);
        }
      }
    }
  }

  // add gpu-cpu links
  for (unsigned d = 0; d < count; ++d) {

    nvmlDevice_t dev;
    NVML(nvmlDeviceGetHandleByIndex(d, &dev));

    // get the CPU affinity
    system::CpuSet cpuset;
    NVML(nvmlDeviceGetCpuAffinity(dev, system::CpuSetSize, cpuset));

    // retrieve sockets with that affinity
    std::vector<Node *> sockets = sys.get_sockets_for_cpuset(cpuset);

    Node *gpu = sys.get_gpu(d);
    if (!gpu) {
      log.error("didn't find GPU %d in system", d);
      continue;
    }

    // attach to sockets
    for (Node *socket : sockets) {
      std::vector<system::System::Path> paths = sys.paths(gpu, socket);
      if (paths.empty()) {
        Link *link = new Link();
        link->type = LinkType::pci;
        link->pci.ancestor = PciAncestor::hostbridge;
        log.info("add pci link gpu %d socket %d", gpu->id, socket->id);
        sys.add_link(gpu, socket, link);
      }
    }
  }
}

} // namespace nvml
} // namespace Realm
