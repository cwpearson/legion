#ifndef REALM_CUDA_NVML_H
#define REALM_CUDA_NVML_H

#include "realm/cuda/system.h"

namespace Realm {
namespace nvml {

/* Add gpus to sys
*/
void add_gpus(Realm::system::System &sys);

/* Add links for gpus found in sys
*/
void add_nvlinks(system::System &sys);


} // namespace nvml
} // namespace Realm

#endif