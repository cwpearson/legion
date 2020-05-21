#ifndef REALM_CUDA_TOPOLOGY_H
#define REALM_CUDA_TOPOLOGY_H

#include <map>
#include <ostream>


namespace Realm {
namespace topology {


enum DistanceKind {
    UNKNOWN = 0,
    SAME, // same device NVML_TOPOLOGY_INTERNAL
    NVLINK_CLOSE, // direct NvLink between GPUs
    NVLINK_FAR, // Path includes NvLink and CPU SMP bus, but no PCIe
    PCIE_CLOSE, // NVML_TOPOLOGY_SINGLE, _MULTIPLE, or _HOSTBRIDGE
    PCIE_FAR, // NVML_TOPOLOGY_NODE and _SYSTEM
};

static const int NVLINK_CLOSE_LATENCY = 260;
static const int NVLINK_CLOSE_BANDWIDTH = 16;
static const int NVLINK_FAR_LATENCY = 320;
static const int NVLINK_FAR_BANDWIDTH = 13;

struct Distance {
    DistanceKind kind;
    unsigned int version; // nvlink version
    unsigned int width; // number of bonded lanes

    static const Distance UNKNOWN_DISTANCE;

    friend std::ostream& operator<<(std::ostream &os, const Distance &d);
};


void lazy_init();


/* Return the link between GPU src and dst
*/
Distance get_gpu_gpu_distance(int src, int dst);


} // namespace nvml
} // namespace Realm

#endif