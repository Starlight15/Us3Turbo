#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

std::string_view ToString(DataFlow flow) {
  switch (flow) {
    case DataFlow::NONE:
      return "none";
    case DataFlow::GPUDirect:
      return "gds-cuobject";
    case DataFlow::CPUDirect:
      return "native-rdma";
  }
  return "unknown";
}

std::string_view ToString(BufferType type) {
  switch (type) {
    case BufferType::kHostRegular:
      return "host-regular";
    case BufferType::kHostPinned:
      return "host-pinned";
    case BufferType::kCudaDevice:
      return "cuda-device";
  }
  return "unknown";
}

std::string_view ToString(OperationType operation) {
  switch (operation) {
    case OperationType::kGet:
      return "GET";
    case OperationType::kPut:
      return "PUT";
    case OperationType::kHead:
      return "HEAD";
  }
  return "UNKNOWN";
}

}  // namespace us3_turbo::client
