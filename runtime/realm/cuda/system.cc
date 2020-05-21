#include <algorithm>
#include <cassert>

#include "realm/cuda/linux.h"
#include "realm/cuda/nvml.h"
#include "realm/cuda/system.h"

#include "realm/logging.h"

namespace Realm {
namespace system {

Logger log_system("system");

const unsigned CPUSET_BYTES = sizeof(CpuSet);
const unsigned CPUSET_BITS = CPUSET_BYTES * 8;
const unsigned CPUSET_FIELD_BITS = sizeof(unsigned long) * 8;

void cpuset_zero(CpuSet *result) {
  for (size_t i = 0; i < sizeof(*result) / sizeof(**result); ++i) {
    (*result)[i] = 0;
  }
}

void cpuset_fill(CpuSet *result) {
  for (size_t i = 0; i < sizeof(*result) / sizeof(**result); ++i) {
    (*result)[i] = 0xFFFFFFFF;
  }
}

void cpuset_set(int i, CpuSet *result) {
  size_t field = i / CPUSET_FIELD_BITS;
  size_t bit = i % CPUSET_FIELD_BITS;
  if (field < 32) {
    (*result)[field] |= (1 << bit);
  }
}

bool cpuset_get(int i, const CpuSet &s) {
  size_t field = i / CPUSET_FIELD_BITS; // bits in a field
  size_t bit = i % CPUSET_FIELD_BITS;
  if (field < 32) {
    return (s[field] >> bit) & 0x1;
  } else {
    return 0;
  }
}

int cpuset_count(const CpuSet &s) {
  int count = 0;
  for (size_t f = 0; f < CpuSetSize; ++f) {
    for (size_t i = 0; i < CPUSET_FIELD_BITS; ++i) {
      if ((s[f] >> i) & 0x1) {
        count++;
      }
    }
  }
  return count;
}

System::~System() {
  for (size_t i = 0; i < nodes.size(); ++i) {
    delete nodes[i];
  }
  for (size_t i = 0; i < links.size(); ++i) {
    delete links[i];
  }
}

void System::init() {
  log_system.info() << "Linux::add_cpus()";
  Linux::add_cpus(*this);
  log_system.info() << "Linux::add_cpu_links()";
  Linux::add_cpu_links(*this);
  log_system.info() << "Linux::add_gpus()";
  nvml::add_gpus(*this);
  log_system.info() << "Linux::add_nvlinks()";
  nvml::add_nvlinks(*this);
}

void System::add_node(Node *node) {
  assert(node);
  auto it = std::find(nodes.begin(), nodes.end(), node);
  if (it != nodes.end()) {
    return;
  } else {
    nodes.push_back(node);
  }
}

Node *System::get_gpu(int i) const {
  for (auto node : nodes) {
    if (node->type == NodeType::gpu && i == node->id) {
      return node;
    }
  }
  return nullptr;
}

std::vector<Link*> System::get_links(const Node *n) const {
  std::vector<Link*> ret;
  for (Link *link : links) {
    if (link->u == n || link->v == n) {
      ret.push_back(link);
    }
  }

  return ret;
}

Link* System::get_link(const Node *u, const Node *v) const {
  std::vector<Link*> ret;
  for (Link *link : links) {
    if (link->u == u || link->v == v) {
      ret.push_back(link);
    }
  }
  assert(ret.size() < 2); // 1 or 0 links
  if (ret.empty()) {
    return nullptr;
  } else {
    return ret.back();
  }

}

  void System::add_link(Node *u, Node *v, Link *link) {
    links.push_back(link);
    if (link->u) {
      assert(link->u == u);
    }
    if (link->v) {
      assert(link->v == v);
    }
    link->u = u;
    link->v = v;
  }

std::vector<Node*> System::get_sockets() const {
  std::vector<Node*> ret;
  for (Node *node : nodes) {
    if (node->type == NodeType::cpu) {
      ret.push_back(node);
    }
  }

  return ret;
}

Node* System::get_socket(int s) const {
  for (Node *node : get_sockets()) {
    if (node->id == s) {
      return node;
    }
  }

  return nullptr;
}

/* return the Node for the socket containing cpu
*/
Node *System::get_socket_for_cpu(int cpu) const {
  std::vector<Node *> sockets = get_sockets();

  for (Node *n : sockets) {
    if (cpuset_get(cpu, n->cpu.cpuset)) {
      return n;
    }
  }
  return nullptr;
}

} // namespace system

} // namespace Realm
