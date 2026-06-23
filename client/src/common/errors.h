#pragma once

#include <string>

#include "us3_turbo/client/result.h"  // Error / MakeError
#include "us3_turbo/client/types.h"   // ErrorCode

namespace us3_turbo::client {

/**
 * @brief client 所有错误构造的唯一入口。
 *
 * 按 @p code 选择 log level 打一次日志,再返回装配好的 Error:
 *   - 调用方错误(kInvalidArgument / kPayloadTooLarge)与可恢复瞬时失败
 *     (kTimeout)→ warn
 *   - 基础设施不可用(RPC / cuObj / channel 失败)→ error
 *
 * 日志在构造点打一次;传播已有 Error 时(直接 Result::Failure(err))
 * 不要调本函数,避免重复日志。failed_path / request_id 仅 GDS 链路
 * (gds-cuobject / 会话 id)填充,纯客户端校验错误留空。
 *
 * @note 字段装配复用 MakeError;本函数只在其上加日志与统一入口。
 */
[[nodiscard]] Error Fail(ErrorCode code,
                         std::string message,
                         bool retryable,
                         std::string failed_path = {},
                         std::string request_id = {});

}  // namespace us3_turbo::client
