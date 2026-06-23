#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

std::string_view ToString(DataFlow flow) {
  switch (flow) {
    case DataFlow::GPUDirect:
      return "gds-cuobject";
  }
  return "unknown";
}

}  // namespace us3_turbo::client
