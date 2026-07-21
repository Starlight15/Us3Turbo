#pragma once

// rdma_put_channel.h — RDMA (libibverbs) 链路的 PutChannel 实现。

#include <cstddef>

#include "client/src/memory_manager/rdma_memory_manager.h"
#include "client/src/transport/put_channel.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/** @brief RDMA 链路的 PutChannel：host 内存走 ibv_reg_mr + EncodeToken，backend
 *   RDMA CM 反向连接后 ibv_post_send(RDMA_READ) 拉取。 */
class RdmaPutChannel final : public PutChannel {
 public:
  RdmaPutChannel(const ClientOptions& options, const ProxyRpc& proxy, RdmaMemoryManager* rdma_mgr)
      : opts_(options), proxy_(proxy), rdma_mgr_(rdma_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& req, ConstBufferView buffer,
                             PutPathResult& res) const override;

  /** @brief 请求校验：检查 buffer 类型、大小合法。 */
  [[nodiscard]] bool ValidateRdmaRequest(const ClientProxyPutRequest& req,
                                          ConstBufferView buffer) const;

 private:
  const ClientOptions& opts_;
  const ProxyRpc& proxy_;
  RdmaMemoryManager* rdma_mgr_;
};

}  // namespace us3_turbo::client
