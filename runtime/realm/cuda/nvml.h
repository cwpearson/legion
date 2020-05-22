#ifndef REALM_CUDA_NVML_H
#define REALM_CUDA_NVML_H

#include "realm/cuda/system.h"

namespace Realm {
namespace nvml {

/* Add gpus to sys
*/
void add_gpus(Realm::system::System &sys);

/* Add nvlinks to system
*/
void add_nvlinks(system::System &sys);

/* Add PCIe links to system
*/

void add_pci(system::System &sys);

} // namespace nvml
} // namespace Realm

#endif