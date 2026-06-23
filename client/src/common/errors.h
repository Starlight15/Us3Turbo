#pragma once

#include <string>
#include <utility>

#include "us3_turbo/client/result.h"  // Error / MakeError
#include "us3_turbo/client/types.h"   // ErrorCode

namespace us3_turbo::client {

/**
 * @brief client 所有错误构造的唯一漏斗。
 *
 * 按 @p code 选 log level 打一次日志,再返回装配好的 Error:
 *   - 调用方错误(kInvalidArgument / kPayloadTooLarge)与可恢复瞬时失败
 *     (kTimeout)→ warn
 *   - 基础设施不可用(RPC / cuObj / channel 失败)→ error
 *
 * failed_path 由 code 推导(GDS 链路 transport/control/timeout → "gds-cuobject",
 * 调用方校验错误 → 空),调用方无需传。传播已有 Error 时直接
 * Result::Failure(err),不要调本函数,避免重复日志。
 *
 * 字段装配复用 MakeError;本函数只在其上加日志与统一入口。
 */
[[nodiscard]] Error Fail(ErrorCode code,
                         std::string message,
                         bool retryable,
                         std::string request_id = {});

/**
 * @brief 固定类别错误助手——调用点糖,内部一律委托 Fail(),日志/level/path
 *        仍只在 Fail 一处,不另开入口。
 * @{
 */
[[nodiscard]] inline Error TransportError(std::string message,
                                          std::string request_id = {}) {
  return Fail(ErrorCode::kTransportError, std::move(message),
              /*retryable=*/true, std::move(request_id));
}
[[nodiscard]] inline Error ControlError(std::string message,
                                        std::string request_id = {}) {
  return Fail(ErrorCode::kControlPlaneError, std::move(message),
              /*retryable=*/true, std::move(request_id));
}
[[nodiscard]] inline Error TimeoutError(std::string message,
                                        std::string request_id = {}) {
  return Fail(ErrorCode::kTimeout, std::move(message),
              /*retryable=*/true, std::move(request_id));
}
[[nodiscard]] inline Error InvalidArgument(std::string message) {
  return Fail(ErrorCode::kInvalidArgument, std::move(message), /*retryable=*/false);
}
[[nodiscard]] inline Error InternalError(std::string message) {
  return Fail(ErrorCode::kInternal, std::move(message), /*retryable=*/true);
}
[[nodiscard]] inline Error PayloadTooLarge(std::string message) {
  return Fail(ErrorCode::kPayloadTooLarge, std::move(message), /*retryable=*/false);
}
/** @} */

}  // namespace us3_turbo::client
