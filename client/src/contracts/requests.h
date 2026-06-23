#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "client/src/common/rpc_context.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief OpenSession RPC 的请求载荷(client-new 自有定义)。
 *
 * 字段与 control_plane.proto::OpenSessionRequest 一一对应;length 为空
 * 时序列化为 expected_size=0。仅保留 GDS PUT 用得到的字段。
 *
 * op_type / data_flow / is_multipart_part 对 GDS-only client 恒为定值
 * ("PUT" / "gds-cuobject" / false),由 RPC 层直接内联,不再作为可变字段。
 */
struct OpenSessionRequest {
  RpcCallMetadata context;
  std::string bucket;
  std::string key;
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  std::string request_id;
  std::string session_id;
};

/**
 * @brief OpenSession 成功后回填的会话元数据;同时作为 GDS PUT 单次传输的
 *        会话句柄直接传递(无需额外包装层)。
 */
struct SessionMeta {
  std::string request_id;
  std::string session_id;
  std::string ticket;
  std::string expire_at;
  std::string gateway_id;
};

/**
 * @brief GdsChunk(GdsPut)RPC 的请求载荷。
 *
 * data_flow 对 GDS-only client 恒为 "gds-cuobject",由 RPC 层内联,
 * 不再作为可变字段。checksum_policy / extra_headers 无服务端消费,已移除。
 */
struct GdsChunkRequest {
  RpcCallMetadata context;
  std::string bucket;
  std::string key;
  std::string request_id;
  std::string session_id;
  std::string transfer_ticket;
  std::string rdma_token;
  std::uint64_t chunk_offset{0};
  std::size_t chunk_size{0};
};

}  // namespace us3_turbo::client
