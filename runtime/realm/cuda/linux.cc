#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "realm/cuda/linux.h"

#include "realm/logging.h"

namespace Realm {
namespace Linux {

Logger log_linux("realm/linux");

using namespace system;

/* break `str` along `delim`
 */
static std::vector<std::string> tokens(const std::string &str,
                                       const char delim) {
  std::vector<std::string> toks;
  std::istringstream f(str);
  std::string tmp;
  while (std::getline(f, tmp, delim)) {
    toks.push_back(tmp);
  }
  return toks;
}

/* convert a string like `0` or `0-2` to a CpuSet
 */
static int cpuset_set_from_range(CpuSet *result, const std::string &s) {
  int ub, lb;
  int ret = sscanf(s.c_str(), "%d-%d", &lb, &ub);
  if (1 == ret) {
    cpuset_set(lb, result);
  } else if (2 == ret) {
    for (int i = lb; i <= ub; ++i) {
      cpuset_set(i, result);
    }
  } else {
    return 1;
  }
  return 0;
}

/* read a file with a contents like `0,1-3,4-10` and convert to a CpuSet
 */
static int read_cpulist_cpuset(CpuSet *result, const char *suffix) {
  std::string path = std::string("/sys/devices/system/cpu/") + suffix;

  std::ifstream t(path.c_str());
  std::stringstream buffer;
  buffer << t.rdbuf();

  std::string str = buffer.str();

  // split into ranges
  std::vector<std::string> ranges = tokens(str, ',');

  for (auto &r : ranges) {
    log_linux.spew() << "range " << r;
    if (!cpuset_set_from_range(result, r)) {
      return 1;
    }
  }
  return 0;
}

/* read a file with ascii unsigned long contents
 */
static int read_ul(unsigned long *u, const char *suffix) {
  std::string path = std::string("/sys/devices/system/cpu/") + suffix;

  std::ifstream t(path.c_str());
  std::stringstream buffer;
  buffer << t.rdbuf();

  int ret = sscanf(buffer.str().c_str(), "%lu", u);
  if (1 == ret) {
    return 0;
  }
  return 1;
}

/* Fill a CpuSet with present cpus, from
   /sys/devices/system/cpu/present
*/
static int get_present_cpus(CpuSet *result) {
  cpuset_zero(result);
  return read_cpulist_cpuset(result, "present");
};

static int get_package_id(unsigned long *id, int i) {
  std::string suffix = "cpu" + std::to_string(i);
  suffix += "/topology/physical_package_id";
  return read_ul(id, suffix.c_str());
}

/* add CPU nodes to the system
https://github.com/karelzak/util-linux/blob/a87f49f6621885ae4e46abf0f510ea21ff90e6c2/sys-utils/lscpu.c#L1080
*/
void add_cpus(system::System &sys) {

  // get present cpus
  CpuSet present = {};
  assert(0 == cpuset_count(present));
  get_present_cpus(&present);
  
  log_linux.info("found %d present cpus", cpuset_count(present));

  // figure out what CPUs are in each socket
  std::map<int, CpuSet> sockets;
  for (size_t i = 0; i < sizeof(CpuSet) * 8; ++i) {
    if (cpuset_get(i, present)) {
      unsigned long ppid;
      if (get_package_id(&ppid, i)) {
        log_linux.error("couldn't get package for cpu %lu", i);
      }
      cpuset_set(i, &sockets[ppid]);
    }
  }

  // if we can't detect any sockets for some reason,
  // just put all CPUs in one socket
  if (sockets.empty()) {
    log_linux.warning() << "couldn't find any sockets.";
    	Node *socket = new Node;
    socket->type = system::NodeType::cpu;
#ifdef __amd64__
    socket->cpu.vendor = system::Vendor::x86;
#elif __powerpc
    socket->cpu.vendor = system::Vendor::ibm;
#endif
    socket->id = 0;
    CpuSet full = {};
    cpuset_fill(&full);
    std::memcpy(socket->cpu.cpuset, full, sizeof(full));
    log_linux.info() << "add socket " << socket->id << " ncpus=" << cpuset_count(socket->cpu.cpuset);
    sys.add_node(socket);
  }

  for (auto &kv : sockets) {
    Node *socket = sys.get_socket(kv.first);
    if (!socket) {
      socket = new Node;
      socket->type = system::NodeType::cpu;
      socket->id = kv.first;
      std::memcpy(socket->cpu.cpuset, kv.second, sizeof(kv.second));
      log_linux.info() << "add socket " << socket->id << " ncpus=" << cpuset_count(socket->cpu.cpuset);
      sys.add_node(socket);
    }
  }
}

void add_cpu_links(system::System &sys) {

    std::vector<Node *> sockets = sys.get_sockets();

    for (size_t i = 0; i < sockets.size(); ++i) {
        for (size_t j = 0; j < sockets.size(); ++j) {
            if (i != j) {
                Link *smp = sys.get_link(sockets[i], sockets[j]);
                if (!smp) {
                    smp = new Link;
                    smp->type = LinkType::smp;
                    sys.add_link(sockets[i], sockets[j], smp);
                }
            }
        }
    }

    }

} // namespace Linux
} // namespace Realm

// https://github.com/karelzak/util-linux/blob/a87f49f6621885ae4e46abf0f510ea21ff90e6c2/sys-utils/lscpu.c#L513
// https://github.com/karelzak/util-linux/blob/a87f49f6621885ae4e46abf0f510ea21ff90e6c2/sys-utils/lscpu.c#L418
