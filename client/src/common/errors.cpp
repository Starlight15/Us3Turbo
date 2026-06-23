#include "client/src/common/errors.h"

#include <utility>

#include <spdlog/spdlog.h>

#include "us3_turbo/common/error_code.h"  // ToString(ErrorCode)
#include "us3_turbo/client/types.h"       // ToString(DataFlow)

namespace us3_turbo::client {

namespace {

// GDS 链路上的失败填 failed_path;纯客户端校验错误(kInvalidArgument /
// kPayloadTooLarge / kInternal)无通路概念,留空。
[[nodiscard]] bool IsPathFailure(ErrorCode c) {
  return c == ErrorCode::kTransportError ||
         c == ErrorCode::kControlPlaneError ||
         c == ErrorCode::kTimeout;
}

}  // namespace

Error Fail(ErrorCode code,
           std::string message,
           bool retryable,
           std::string request_id) {
  // 调用方错误 + 超时 → warn;基础设施不可用 → error。
  const bool caller_fault = (code == ErrorCode::kInvalidArgument ||
                             code == ErrorCode::kPayloadTooLarge);
  const auto lvl = (caller_fault || code == ErrorCode::kTimeout)
                       ? spdlog::level::warn
                       : spdlog::level::err;
  const std::string failed_path =
      IsPathFailure(code) ? std::string(ToString(DataFlow::GPUDirect)) : std::string{};
  spdlog::log(lvl,
              "us3_turbo client error: code={} retryable={} path={} req={} | {}",
              us3_turbo::common::ToString(code),
              retryable,
              failed_path,
              request_id,
              message);
  return MakeError(code, std::move(message), retryable,
                   std::move(failed_path), std::move(request_id));
}

}  // namespace us3_turbo::client
