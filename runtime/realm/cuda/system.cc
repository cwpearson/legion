#include <algorithm>
#include <cassert>
#include <iostream>

#include "realm/cuda/linux.h"
#include "realm/cuda/nvml.h"
#include "realm/cuda/system.h"

#include "realm/logging.h"

namespace Realm {
namespace system {

Logger log_system("system");

const unsigned CPUSET_BYTES = sizeof(CpuSet);
const unsigned CPUSET_BITS = CPUSET_BYTES * 8;
const unsigned CpuSetFieldBits = sizeof(CpuSetField) * 8;

void cpuset_zero(CpuSet *result) {
  for (size_t i = 0; i < CpuSetSize; ++i) {
    (*result)[i] = 0;
  }
}

void cpuset_fill(CpuSet *result) {
  for (size_t i = 0; i < CpuSetSize; ++i) {
    (*result)[i] = 0xFFFFFFFF;
  }
}

void cpuset_set(int i, CpuSet *result) {
  size_t field = i / CpuSetFieldBits;
  size_t bit = i % CpuSetFieldBits;
  if (field < 32) {
    (*result)[field] |= (1ul << bit);
  }
}

bool cpuset_get(int i, const CpuSet &s) {
  assert(i < sizeof(CpuSet) * 8);
  size_t field = i / CpuSetFieldBits; // bits in a field
  size_t bit = i % CpuSetFieldBits;
  if (field < 32) {
    return (s[field] >> bit) & 0x1;
  } else {
    return 0;
  }
}

int cpuset_count(const CpuSet &s) {
  int count = 0;
  for (size_t f = 0; f < CpuSetSize; ++f) {
    for (size_t i = 0; i < CpuSetFieldBits; ++i) {
      if ((s[f] >> i) & 0x1) {
        count++;
      }
    }
  }
  return count;
}

void cpuset_intersection(CpuSet *result, const CpuSet &x, const CpuSet &y) {
  cpuset_zero(result);
  for (size_t i = 0; i < CpuSetSize; ++i) {
    (*result)[i] = x[i] & y[i];
  }
}

Link::Link() : u(nullptr), v(nullptr) {}

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
  log_system.info() << "Linux::add_pci()";
  nvml::add_pci(*this);
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

std::vector<Link *> System::get_links(const Node *n) const {
  std::vector<Link *> ret;
  for (Link *link : links) {
    if (link->u == n || link->v == n) {
      ret.push_back(link);
    }
  }

  return ret;
}

Link *System::get_link(const Node *u, const Node *v) const {
  std::vector<Link *> ret;
  for (Link *link : links) {
    if (link->u == u && link->v == v) {
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

std::vector<Node *> System::get_sockets() const {
  std::vector<Node *> ret;
  for (Node *node : nodes) {
    if (node->type == NodeType::cpu) {
      ret.push_back(node);
    }
  }

  return ret;
}

Node *System::get_socket(int s) const {
  for (Node *node : get_sockets()) {
    if (node->id == s) {
      return node;
    }
  }

  return nullptr;
}

Node *System::get_socket_for_cpu(int cpu) const {
  std::vector<Node *> sockets = get_sockets();

  for (Node *n : sockets) {
    if (cpuset_get(cpu, n->cpu.cpuset)) {
      return n;
    }
  }
  return nullptr;
}

std::vector<Node *> System::get_sockets_for_cpuset(const CpuSet &s) const {
  std::vector<Node *> matches;
  for (Node *n : get_sockets()) {
    CpuSet i;
    cpuset_intersection(&i, n->cpu.cpuset, s);
    if (cpuset_count(i)) {
      matches.push_back(n);
    }
  }
  return matches;
}

} // namespace system

} // namespace Realm
