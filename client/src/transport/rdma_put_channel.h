#pragma once

// rdma_put_channel.h — RDMA (libibverbs) 链路 PUT channel。

#include <cstddef>

#include "client/src/common/request.h"
#include "client/src/memory_manager/rdma_memory_manager.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/*
 * RDMA PutChannel：host 内存走 ibv_reg_mr + EncodeToken，backend RDMA CM 反向连接后 RDMA-READ。
 */
class RdmaPutChannel final {
 public:
  RdmaPutChannel(const ClientOptions& options, const ProxyRpc& proxy, RdmaMemoryManager* rdma_mgr)
      : opts_(options), proxy_(proxy), rdma_mgr_(rdma_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& req, ConstBufferView buffer,
                             PutPathResult& res) const;

 private:
  [[nodiscard]] bool ValidateRdmaRequest(const ClientProxyPutRequest& req, ConstBufferView buffer) const;

  const ClientOptions& opts_;
  const ProxyRpc& proxy_;
  RdmaMemoryManager* rdma_mgr_;
};

}  // namespace us3_turbo::client
