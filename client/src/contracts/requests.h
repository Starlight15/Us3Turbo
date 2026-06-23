#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief OpenSession RPC 的请求载荷(client-new 自有定义)。
 *
 * 字段与 control_plane.proto::OpenSessionRequest 对应;length 为空时序列化
 * 为 expected_size=0。仅保留 proxy 实际消费的字段(proxy_control_plane_service
 * 只读 session_id / bucket / object_key / expected_size);offset 服务端从不
 * 读、client 恒为 0,已移除。
 *
 * op_type / data_flow / is_multipart_part 对 GDS-only client 恒为定值
 * ("PUT" / "gds-cuobject" / false),由 RPC 层直接内联,不再作为可变字段。
 */
struct OpenSessionRequest {
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::string bucket;
  std::string key;
  std::optional<std::uint64_t> length;
  std::string request_id;
  std::string session_id;
};

/**
 * @brief OpenSession 成功后回填的会话元数据;同时作为 GDS PUT 单次传输的
 *        会话句柄直接传递(无需额外包装层)。
 *
 * 只保留 client 后续实际用到的字段:ticket 用于 GdsChunk、request_id /
 * session_id 回填到 outcome。gateway_id / expire_at 虽由 proxy 回填,但
 * client 从不读取,已移除。
 */
struct SessionMeta {
  std::string request_id;
  std::string session_id;
  std::string ticket;
};

/**
 * @brief GdsChunk(GdsPut)RPC 的请求载荷。
 *
 * 仅保留 backend 实际消费的字段(backend_data_plane_service 读 bucket /
 * object_key / rdma_token / chunk_size / session_id);chunk_offset 服务端
 * 从不读、client 恒为 0,已移除。data_flow 对 GDS-only client 恒为
 * "gds-cuobject",由 RPC 层内联,不再作为可变字段。
 */
struct GdsChunkRequest {
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::string bucket;
  std::string key;
  std::string request_id;
  std::string session_id;
  std::string transfer_ticket;
  std::string rdma_token;
  std::size_t chunk_size{0};
};

}  // namespace us3_turbo::client
