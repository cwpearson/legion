#ifndef REALM_CUDA_SYSTEM_H
#define REALM_CUDA_SYSTEM_H

#include <vector>
#include <bitset>
#include <set>
#include <deque>
#include <algorithm>
#include <cassert>
#include <functional>

namespace Realm {
namespace system {


typedef unsigned long CpuSet[32];
static const unsigned CpuSetSize = 32;

/* zero a CpuSet
 */
void cpuset_zero(CpuSet *result);

/* make all bits 1
 */
void cpuset_fill(CpuSet *result);

/* set bit `i` in a CpuSet
 */
void cpuset_set(int i, CpuSet *result);

/* set bit `i` in a CpuSet
 */
bool cpuset_get(int i, const CpuSet &s);

/* count the active bits in a CpuSet
 */
int cpuset_count(const CpuSet &s);




enum LinkType {
  pci,
  nvlink,
  smp,
};

enum Vendor {
  ibm,
  x86,
};

enum NodeType {
  cpu,
  gpu,
  nvswitch,
  pcitree,
};

struct Node;
struct Link {
  int type;
  Node *u;
  Node *v;
  union {
    struct {
      unsigned int version;
      int width;
    } nvlink;
  };
};

struct Node {
  int type;
  int id;
  union {
    struct {
    } gpu;
    struct {
      int vendor;
      CpuSet cpuset;
    } cpu;
  };
};

class System {
public:
  typedef std::vector<Link *> Path;

private:
  std::vector<Node *> nodes;
  std::vector<Link *> links;

public:
  ~System();

  void init();
  Node *get_gpu(int id) const;
  void add_node(Node *node);

  std::vector<Link*> get_links(const Node *n) const;

  /* return a link connecting u and v, or nullptr
   */
  Link *get_link(const Node *u, const Node *v) const;

  /* add a link between node u and v
  */
  void add_link(Node *u, Node *v, Link *link);

  /* return the Node for socket s;
   */
  Node *get_socket(int s) const;

  /* return the Node for the socket containing cpu
   */
  Node *get_socket_for_cpu(int cpu) const;
 
  /* return all socket nodes
  */
  std::vector<Node *> get_sockets() const;

  /*
  return all paths from src to dst
*/
  std::vector<Path> paths(const Node * src, const Node * dst) {

    std::vector<Path> ret; // the paths from src to dst

    if (nodes.empty()) {
      assert(links.empty());
      return ret;
    }

    std::set<Link *> visited; // the edges we have traversed
    std::deque<Path> worklist;

    // std::cerr << "init worklist " << src->edges_.size() << "\n";

    // initialize worklist
    for (auto e : get_links(src)) {
      Path path = {e};
      worklist.push_front(path);
      visited.insert(e);
    }

    while (!worklist.empty()) {
      // std::cerr << "worklist.size()=" << worklist.size() << "\n";
      // std::cerr << "visited.size()=" << visited.size() << "\n";

      Path next = worklist.back();
      worklist.pop_back();

      if (next.back()->u == dst || next.back()->v == dst) {
        ret.push_back(next);
      } else {
        // make new paths for work list
        Node * u = next.back()->u;
        for (auto e : get_links(u)) {
          Path path = next;
          if (0 == visited.count(e)) {
            path.push_back(e);
            visited.insert(e);
            worklist.push_back(path);
          }
        }

        Node * v = next.back()->v;
        for (auto e : get_links(v)) {
          Path path = next;
          if (0 == visited.count(e)) {
            path.push_back(e);
            visited.insert(e);
            worklist.push_back(path);
          }
        }
      }
    }

    return ret;
  }

  // return the path from src to dst that has the minimum cost.
  // if no path is found, return an empty path
  // CostFunc takes a const ref to a Path and returns a cost
  template <typename CostFunc>
  Path min_path(const Node *src, const Node *dst, CostFunc cost) {
    std::vector<Path> allPaths = paths(src, dst);
    if (allPaths.empty()) {
      return Path();
    }
    auto path_cmp = [&](const Path &a, const Path &b) {
      return cost(a) < cost(b);
    };
    auto it = std::min_element(allPaths.begin(), allPaths.end(), path_cmp);
    assert(it != allPaths.end());
    return *it;
  }


};

} // namespace system
} // namespace Realm

#endif