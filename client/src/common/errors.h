#pragma once

#include <string_view>

#include "us3_turbo/client/result.h"
#include "us3_turbo/client/types.h"  // ToString(DataFlow)

namespace us3_turbo::client {

/**
 * @brief client-new 共享的错误构造工厂。
 *
 * 统一 ErrorCode 选择 / retryable 标记 / failed_path 填充,避免各处自行拼接。
 * 只保留 GDS PUT 链路实际用到的几种;header-only,三处都是单行工厂。
 */

[[nodiscard]] inline Error MakeNotInitialized(std::string_view component) {
  return MakeError(ErrorCode::kInternal,
                   std::string(component) + " is not initialized. Call Client::Initialize first.",
                   /*retryable=*/true);
}

[[nodiscard]] inline Error MakeInvalidArgument(std::string_view message) {
  return MakeError(ErrorCode::kInvalidArgument, std::string(message));
}

[[nodiscard]] inline Error MakeTransportFailure(std::string_view message,
                                                DataFlow path,
                                                std::string_view request_id,
                                                bool retryable) {
  return MakeError(ErrorCode::kTransportError, std::string(message), retryable,
                   std::string(ToString(path)), std::string(request_id));
}

}  // namespace us3_turbo::client
