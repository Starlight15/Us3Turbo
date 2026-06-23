#include "client/src/common/errors.h"

#include <utility>

#include "us3_turbo/client/types.h"  // ToString(DataFlow)

namespace us3_turbo::client {

Error MakeNotInitialized(std::string_view component) {
  return MakeError(ErrorCode::kInternal,
                   std::string(component) + " is not initialized. Call Client::Initialize first.",
                   /*retryable=*/true);
}

Error MakeInvalidArgument(std::string_view message) {
  return MakeError(ErrorCode::kInvalidArgument, std::string(message));
}

Error MakeUnsupportedPath(DataFlow path, std::string_view message) {
  return MakeError(ErrorCode::kUnsupported, std::string(message), /*retryable=*/false,
                   std::string(ToString(path)));
}

Error MakeTransportFailure(std::string_view message,
                           DataFlow path,
                           std::string_view request_id,
                           bool retryable) {
  return MakeError(ErrorCode::kTransportError, std::string(message), retryable,
                   std::string(ToString(path)), std::string(request_id));
}

}  // namespace us3_turbo::client
