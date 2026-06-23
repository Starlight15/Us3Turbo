#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "client/src/common/rpc_context.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief OpenSession RPC 的请求载荷(client-new 自有定义)。
 *
 * 字段与 control_plane.proto::OpenSessionRequest 一一对应;length 为空
 * 时序列化为 expected_size=0。仅保留 GDS PUT 用得到的字段。
 */
struct OpenSessionRequest {
  RpcCallMetadata context;
  OperationType operation{OperationType::kPut};
  std::string bucket;
  std::string key;
  DataFlow data_flow{DataFlow::GPUDirect};
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  std::string request_id;
  std::string session_id;
  std::string idempotency_key;
  bool is_multipart_part{false};
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
 */
struct GdsChunkRequest {
  RpcCallMetadata context;
  OperationType operation{OperationType::kPut};
  std::string bucket;
  std::string key;
  DataFlow data_flow{DataFlow::GPUDirect};
  std::string checksum_policy{"none"};
  std::unordered_map<std::string, std::string> extra_headers;
  std::string request_id;
  std::string session_id;
  std::string transfer_ticket;
  std::string rdma_token;
  std::uint64_t chunk_offset{0};
  std::size_t chunk_size{0};
};

}  // namespace us3_turbo::client
