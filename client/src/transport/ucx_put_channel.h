#pragma once

// ucx_put_channel.h — UCX 链路(底层走 RDMA)的 PutChannel 实现。

#include <cstddef>

#include "client/src/memory_manager/ucx_memory_manager.h"
#include "client/src/transport/put_channel.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/** @brief UCX 链路的 PutChannel:host 内存走 ucp_mem_map + packed rkey,backend ucp_get_nbx 反向拉取。 */
class UcxPutChannel final : public PutChannel {
 public:
  UcxPutChannel(const ClientOptions& options, const ProxyRpc& proxy,
                UcxMemoryManager* ucx_mgr)
      : options_(options), proxy_(proxy), ucx_mgr_(ucx_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const override;

 private:
  const ClientOptions&   options_;
  const ProxyRpc&        proxy_;
  UcxMemoryManager*      ucx_mgr_;
};

}  // namespace us3_turbo::client
