#pragma once

// gds_put_channel.h — GDS (CUDA cuObj RDMA) 链路 PutChannel。

#include <cstddef>

#include "client/src/memory_manager/gds_memory_manager.h"
#include "client/src/transport/put_channel.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/*
 * GDS PutChannel：GPU 显存走 cuObj RDMA token，backend 反向 RDMA-READ。
 */
class GdsPutChannel final : public PutChannel {
 public:
  GdsPutChannel(const ClientOptions& options, const ProxyRpc& proxy, GdsMemoryManager* gds_mgr)
      : opts_(options), proxy_(proxy), gds_mgr_(gds_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& req, ConstBufferView buffer,
                             PutPathResult& res) const override;

  [[nodiscard]] bool ValidateGdsRequest(const ClientProxyPutRequest& req,
                                         ConstBufferView buffer) const;

 private:
  const ClientOptions& opts_;
  const ProxyRpc& proxy_;
  GdsMemoryManager* gds_mgr_;
};

}  // namespace us3_turbo::client
