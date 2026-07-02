#pragma once

// gds_put_channel.h — GDS(CUObj RDMA)链路的 PutChannel 实现,与 ucx_put_channel.* 物理隔离。
// PutOnce:AcquireToken → GdsDataSource → proxy.GdsPut → 可选 CRC(D2H)/trace。

#include <cstddef>

#include "client/src/memory_manager/gds_memory_manager.h"
#include "client/src/transport/put_channel.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/**
 * @brief GDS 链路的 PutChannel。device 显存走 cuObj RDMA token + backend
 *        反向 RDMA-READ。构造持有的引用须由 Client 保活到本对象销毁。
 *        buffer 注册在 AcquireToken 内懒注册,对外无注册 API。
 */
class GdsPutChannel final : public PutChannel {
 public:
  GdsPutChannel(const ClientOptions& options, const ProxyRpc& proxy,
                GdsMemoryManager* gds_mgr)
      : options_(options), proxy_(proxy), gds_mgr_(gds_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const override;

 private:
  const ClientOptions&  options_;
  const ProxyRpc&        proxy_;
  GdsMemoryManager*      gds_mgr_;
};

}  // namespace us3_turbo::client
