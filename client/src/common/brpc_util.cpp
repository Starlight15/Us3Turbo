#include "client/src/common/brpc_util.h"

#include <utility>

#include <brpc/errno.pb.h>

#include "us3_turbo/client/result.h"  // Error / MakeError

namespace us3_turbo::client {
namespace {

// 去尾斜杠,让 "host:port/" 与 "host:port" 等价。
[[nodiscard]] std::string TrimTrailingSlash(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint;
}

}  // namespace

void ApplyRequestHeaders(brpc::Controller& controller, const RpcCallMetadata& context) {
  controller.set_timeout_ms(static_cast<int>(context.timeout.count()));
}

Result<bool> CheckRpcFailure(const brpc::Controller& controller,
                             std::string_view message,
                             DataFlow path,
                             std::string_view request_id,
                             bool is_data_plane) {
  if (!controller.Failed()) {
    return Result<bool>::Success(true);
  }
  const int err = controller.ErrorCode();
  const bool is_timeout = (err == brpc::ERPCTIMEDOUT) || (err == ETIMEDOUT);
  const ErrorCode code = is_timeout
                             ? ErrorCode::kTimeout
                             : (is_data_plane ? ErrorCode::kTransportError
                                              : ErrorCode::kControlPlaneError);
  return Result<bool>::Failure(MakeError(
      code,
      std::string(message) + ": " + controller.ErrorText(),
      /*retryable=*/true,
      std::string(ToString(path)),
      std::string(request_id)));
}

std::unique_ptr<brpc::Channel> InitBrpcChannel(const std::string& endpoint,
                                               std::chrono::milliseconds timeout,
                                               std::string& out_error,
                                               std::string_view role) {
  if (endpoint.empty()) {
    out_error = role.empty()
                    ? std::string{"endpoint must not be empty"}
                    : std::string{role} + " endpoint must not be empty";
    return nullptr;
  }
  // 就地 Init 一条 baidu_std channel:connect / RPC 超时同源,max_retry=2。
  auto channel = std::make_unique<brpc::Channel>();
  brpc::ChannelOptions co;
  co.protocol           = "baidu_std";
  co.connect_timeout_ms = static_cast<int>(timeout.count());
  co.timeout_ms         = static_cast<int>(timeout.count());
  co.max_retry          = 2;
  if (channel->Init(TrimTrailingSlash(endpoint).c_str(), nullptr, &co) != 0) {
    out_error = "Failed to initialize brpc channel: " + endpoint;
    return nullptr;
  }
  return channel;
}

}  // namespace us3_turbo::client
