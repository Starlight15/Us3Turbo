#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace us3_turbo::client {

/** @brief PUT 通路选择。普通枚举（非 bitflag）：v1 仅单路。 */
enum class PutDataPath : std::uint8_t {
  kNone = 0,
  kGds = 1,
  kRdma = 2,
};

/** @brief client → proxy 统一 PUT 请求;对应通路的 rdma_token 由 PutObject
 * 内部按 path 填充。 */
struct ClientProxyPutRequest {
  std::string req_id;
  std::string bucket;
  std::string key;
  std::uint64_t object_size{0};
  PutDataPath path{PutDataPath::kNone};
};

/** @brief 单条通路的执行结果。ok=false 时 error_code/error_message 描述失败原因。
 */
struct PutPathResult {
  bool ok{false};
  std::int32_t error_code{0};
  std::string error_message;
  std::string etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes{0};
};

/** @brief proxy → client 统一 PUT 响应，各通路结果按 path 独立返回。 */
struct ClientProxyPutResponse {
  std::optional<PutPathResult> gds_result;
  std::optional<PutPathResult> rdma_result;
};

// ========== GET（GDS）控制面消息 ==========

/** @brief GET 执行结果：按块读取 + crc 重组校验后的结果。 */
struct GetPathResult {
  bool ok{false};
  std::int32_t error_code{0};
  std::string error_message;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_read{0};
  std::string hash;
};

}  // namespace us3_turbo::client
