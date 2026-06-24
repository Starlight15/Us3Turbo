#pragma once

#include <chrono>
#include <string>

#include "client/src/common/rpc_base.h"
#include "client/src/contracts/requests.h"

namespace us3_turbo::client {

/**
 * @brief 数据面 RPC client(→ backend)。
 *
 * protobuf 响应类型只在 chunk_rpc.cpp 内部出现,对外只回填客户端自有的
 * GdsPutResult。
 */
class ChunkRpc : public RpcBase {
 public:
  ChunkRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "data") {}

  [[nodiscard]] bool Put(const GdsChunkRequest& request,
                        GdsPutResult& out) const;
};

}  // namespace us3_turbo::client
