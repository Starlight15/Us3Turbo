#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace us3_turbo::client {

/** @brief PUT 通路选择(bitflags)。kNone=0 作无效默认,kAll 暂不支持(单 buffer
 * 无法双路)。 */
enum class PutDataPath : std::uint8_t {
  kNone = 0,
  kGds = 1 << 0,
  kUcx = 1 << 1,
  kAll = kGds | kUcx,
};

inline PutDataPath operator|(PutDataPath a, PutDataPath b) {
  return static_cast<PutDataPath>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

inline bool HasPath(PutDataPath flags, PutDataPath check) {
  return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(check)) != 0;
}

/** @brief GDS 通路数据源:cuObj RDMA token(显存地址 + remote key 自描述串)。 */
struct GdsDataSource {
  std::string rdma_token;
};

/** @brief UCX 通路数据源:host buffer 虚拟地址 + packed rkey + client UCX
 * listener 地址。 */
struct UcxDataSource {
  std::uint64_t remote_addr{0};
  std::string packed_rkey;
  std::string client_ucx_addr;
};

/** @brief client → proxy 统一 PUT 请求;对应通路 source 由 PutObject 内部按 path
 * 填充。 */
struct ClientProxyPutRequest {
  std::string req_id;
  std::string bucket;
  std::string key;
  std::uint64_t object_size{0};
  PutDataPath path{PutDataPath::kNone};
  std::optional<GdsDataSource> gds_source;
  std::optional<UcxDataSource> ucx_source;
};

/** @brief 单条通路的执行结果。ok=false 时 error_code/error_message 描述失败。
 */
struct PutPathResult {
  bool ok{false};
  std::int32_t error_code{0};
  std::string error_message;
  std::string etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

/** @brief proxy → client 统一 PUT 响应,各通路结果按 path 独立返回。 */
struct ClientProxyPutResponse {
  std::string object_id;
  std::optional<PutPathResult> gds_result;
  std::optional<PutPathResult> ucx_result;
};

// ========== GET（GDS）控制面消息 ==========

/** @brief StatObject 输出：对象布局，供 client 分配 buffer。 */
struct StatObjectOutput {
  std::uint64_t object_size{0};
  std::uint64_t block_size{0};
  std::string hash;
};

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
