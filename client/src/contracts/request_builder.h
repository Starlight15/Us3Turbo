#pragma once

#include <chrono>
#include <string_view>

#include "client/src/contracts/requests.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief client-new 的请求 / 结果装配工厂。
 *
 * 单对象 PUT 的固定语义(op=PUT、data_flow=GPUDirect、chunk_size=buffer.size)
 * 由调用侧钉死,工厂只把 PutObjectRequest + buffer 折叠成一次尝试上下文
 * (PutAttempt)与最终结果(TransferOutcome),字段从源头复制一次,不再手写
 * 中间结构。本头文件不依赖任何 protobuf 类型。
 */

/** @brief 装配 PutAttempt:timeout/bucket/key/length 来自 PutObjectRequest,生成新 id。 */
[[nodiscard]] PutAttempt MakePutAttempt(const ClientOptions& options,
                                        const PutObjectRequest& request);

/**
 * @brief 从尝试上下文 + GdsPut 结果装配 TransferOutcome:回填 etag +
 *        bytes_transferred(buffer.size)+ attempt 的 request_id。
 */
[[nodiscard]] TransferOutcome MakeTransferOutcome(const PutAttempt& attempt,
                                                  const GdsPutResult& result,
                                                  ConstBufferView buffer);

}  // namespace us3_turbo::client
