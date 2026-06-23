#pragma once

#include <string_view>

#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief client-new 共享的错误构造工厂。
 *
 * 统一 ErrorCode 选择 / retryable 标记 / failed_path 填充,避免各处自行拼接。
 * 只保留 GDS PUT 链路实际用到的几种;不沿用旧 client 的 errors.h 全集。
 */

[[nodiscard]] Error MakeNotInitialized(std::string_view component);
[[nodiscard]] Error MakeInvalidArgument(std::string_view message);
[[nodiscard]] Error MakeTransportFailure(std::string_view message,
                                         DataFlow path,
                                         std::string_view request_id,
                                         bool retryable);

}  // namespace us3_turbo::client
