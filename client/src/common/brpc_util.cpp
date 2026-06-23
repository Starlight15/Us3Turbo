#include "client/src/common/brpc_util.h"

#include <utility>

#include <brpc/errno.pb.h>

#include "us3_turbo/client/result.h"  // Error / MakeError

namespace us3_turbo::client {

void ApplyRequestHeaders(brpc::Controller& controller, const RpcCallMetadata& context) {
  for (const auto& [key, value] : context.default_headers) {
    controller.http_request().SetHeader(key, value);
  }
  if (!context.client_id.empty()) {
    controller.http_request().SetHeader("x-fa-client-id", context.client_id);
  }
  if (!context.bearer_token.empty()) {
    controller.http_request().SetHeader("Authorization", "Bearer " + context.bearer_token);
  }
  controller.set_timeout_ms(static_cast<int>(context.timeout.count()));
}

Result<bool> CheckRpcFailure(const brpc::Controller& controller,
                             std::string_view message,
                             DataFlow path,
                             std::string_view request_id) {
  if (!controller.Failed()) {
    return Result<bool>::Success(true);
  }
  const int err = controller.ErrorCode();
  const bool is_timeout = (err == brpc::ERPCTIMEDOUT) || (err == ETIMEDOUT);
  return Result<bool>::Failure(MakeError(
      is_timeout ? ErrorCode::kTimeout : ErrorCode::kControlPlaneError,
      std::string(message) + ": " + controller.ErrorText(),
      /*retryable=*/true,
      std::string(ToString(path)),
      std::string(request_id)));
}

}  // namespace us3_turbo::client
