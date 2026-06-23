#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief Client instance configuration.
 */
struct ClientOptions {
  /** 控制面 endpoint "host:port"（→ proxy）。 */
  std::string endpoint;
  /** Identifier reported to the gateway for telemetry and logs. */
  std::string client_id{"us3-turbo-access-client"};
  /** Optional bearer token included in outbound requests. */
  std::string bearer_token;
  /** Headers attached to every outbound RPC request. */
  std::unordered_map<std::string, std::string> default_headers;
  /** Default per-channel timeout（用于 MetaRpc / ChunkRpc channel Init）。 */
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};
  /** 端到端单笔 PUT 超时（OpenSession + GdsPut 总体限时）。 */
  std::chrono::milliseconds request_timeout{std::chrono::minutes(5)};
  /**
   * GDS 数据面（GdsPut）目标 "host:port"，指向独立 backend 进程。
   *   endpoint          = 控制面（proxy，OpenSession 走这里）
   *   gds_data_endpoint = 数据面（backend，GdsPut 走这里）
   */
  std::string gds_data_endpoint;

  /**
   * 单次 GDS PUT 的上限（client 入口拒）。与 gateway 端 cuObjServer 的
   * 1 GiB chunk 上限对齐，避免发到 server 才被拒。0 表示不限。
   */
  std::size_t put_single_max_bytes{1ULL * 1024 * 1024 * 1024};
};

}  // namespace us3_turbo::client
