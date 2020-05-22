

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <algorithm>

#include "realm/logging.h"


#include "realm/cuda/topology.h"
#include "realm/cuda/system.h"

namespace Realm {
namespace topology {


Logger log_topology("topology");


system::System sys;

void lazy_init() {

  static bool done = false;
  if (!done) {
    sys.init();
    done = true;
  }

}

/*static*/ const Distance Distance::UNKNOWN_DISTANCE = {};

int length(const system::System::Path &path) { return path.size(); }

Distance get_gpu_gpu_distance(int srcId, int dstId) {

  using system::Node;
  using system::Link;
  using system::LinkType;

  Node *src = sys.get_gpu(srcId);
  Node *dst = sys.get_gpu(dstId);
  assert(src);
  assert(dst);

  if (srcId == dstId) {
    Distance d;
    d.kind = DistanceKind::SAME;
    return d;
  }

  log_topology.spew() << "about to look for path";

  std::vector<Link *> path = sys.min_path(src, dst, length);

  log_topology.info() << path.size();

  // if the path is a single nvlink
  if (path.size() == 1 && path[0]->type == LinkType::nvlink) {
    Distance d;
    d.kind = DistanceKind::NVLINK_CLOSE;
    d.version = path[0]->nvlink.version;
    d.width = path[0]->nvlink.width;
    return d;
  }

  // if the path contains nvlink
  if (std::any_of(path.begin(), path.end(), [](Link *l){return l->type == LinkType::nvlink;})) {
    Distance d;
    d.kind = DistanceKind::NVLINK_FAR;
    return d;
  }

  // TODO: no PCIe tree yet, so just return unknown otherwise

  return Distance::UNKNOWN_DISTANCE;

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
