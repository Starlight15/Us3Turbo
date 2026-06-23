#pragma once

#include <string>
#include <string_view>

#include <brpc/controller.h>

#include "client/src/common/rpc_context.h"
#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief 把 RpcCallMetadata 应用到一个 brpc::Controller:
 *        default_headers → HTTP header;client_id → x-fa-client-id;
 *        bearer_token → Authorization;timeout → set_timeout_ms。
 */
void ApplyRequestHeaders(brpc::Controller& controller, const RpcCallMetadata& context);

/**
 * @brief 统一检查 brpc 调用是否失败,失败时构造带 ErrorCode 的 Error。
 *
 * brpc 超时 (ERPCTIMEDOUT / ETIMEDOUT) → kTimeout(retryable=true);其它
 * 失败 → kControlPlaneError。成功返回 Success(true)。
 */
[[nodiscard]] Result<bool> CheckRpcFailure(const brpc::Controller& controller,
                                           std::string_view message,
                                           DataFlow path,
                                           std::string_view request_id);

}  // namespace us3_turbo::client
