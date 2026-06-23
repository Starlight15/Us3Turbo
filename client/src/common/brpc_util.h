#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "client/src/common/rpc_context.h"
#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief 把 RpcCallMetadata 应用到一个 brpc::Controller:timeout → set_timeout_ms。
 *        (鉴权 / 自定义 header 字段已随 RpcCallMetadata 一并移除。)
 */
void ApplyRequestHeaders(brpc::Controller& controller, const RpcCallMetadata& context);

/**
 * @brief 统一检查 brpc 调用是否失败,失败时构造带 ErrorCode 的 Error。
 *
 * brpc 超时 (ERPCTIMEDOUT / ETIMEDOUT) → kTimeout(retryable=true)。
 * 非超时失败:is_data_plane=true → kTransportError(数据面);否则
 * kControlPlaneError(控制面)。成功返回 Success(true)。
 */
[[nodiscard]] Result<bool> CheckRpcFailure(const brpc::Controller& controller,
                                           std::string_view message,
                                           DataFlow path,
                                           std::string_view request_id,
                                           bool is_data_plane = false);

/**
 * @brief 构造并 Init 一条 baidu_std brpc::Channel,供 RPC stub 复用。
 *
 * 内部统一处理:trim 尾部斜杠、connect/RPC 超时同源(timeout)、max_retry=2。
 * 失败时 @p out_error 填入原因并返回 nullptr(不抛出)。@p endpoint 为空
 * 直接失败。
 *
 * @param role 可选角色前缀(如 "control" / "data"),用于在 @p endpoint 为空
 *             的失败消息中区分控制面 / 数据面(非空时形如 "control endpoint
 *             must not be empty");为空时退回通用消息。
 */
[[nodiscard]] std::unique_ptr<brpc::Channel>
InitBrpcChannel(const std::string& endpoint,
                std::chrono::milliseconds timeout,
                std::string& out_error,
                std::string_view role = {});

}  // namespace us3_turbo::client
