#ifndef REALM_CUDA_LINUX_H
#define REALM_CUDA_LINUX_H

#include "realm/cuda/system.h"

namespace Realm {
    //TODO: "linux" is a numeric constant?
namespace Linux {

/* add CPU nodes
*/
void add_cpus(system::System &sys);

/* add SMP links between CPUs
*/
void add_cpu_links(system::System &sys);

} // namespace lnx
} // namespace Realm

#endif