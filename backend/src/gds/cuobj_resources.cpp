#include "gds/cuobj_resources.h"

#include <cstdlib>

namespace us3_turbo::gateway::data_flow::gds {

HostBuffer::~HostBuffer() {
  if (data_ != nullptr) {
    std::free(data_);
  }
}

}  // namespace us3_turbo::gateway::data_flow::gds
