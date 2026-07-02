#pragma once

// ucx_put_channel.h — RDMA(UCX)链路的 PutChannel 实现。
//
// 与 gds_put_channel.* 物理隔离(互不 include),可单独回归——UCX 链路不能
// 被 GDS 依赖拖累(见 review/client_refactor_prompt.md 硬约束)。
//
// 持有 ClientOptions / ProxyRpc / UcxMemoryManager 的引用/指针(均由 Client
// 在 Initialize 时传入并保活),PutOnce 搬运原 client.cpp 的 UcxPutOnce 逻辑:
// AcquireDescriptor(host) → 构 UcxDataSource → proxy.UcxPut → 回填 result →
// 可选 VerifyUcxCrc32c(host 直算)→ 可选 UCX trace。

#include <cstddef>

#include "client/src/rdma_transport/ucx_memory_manager.h"
#include "client/src/transport/put_channel.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/**
 * @brief UCX 链路的 PutChannel。host 内存走 ucp_mem_map + packed rkey,
 *        backend ucp_get_nbx 反向拉取。构造持有的引用必须由 Client 保活。
 */
class UcxPutChannel final : public PutChannel {
 public:
  UcxPutChannel(const ClientOptions& options, const ProxyRpc& proxy,
                UcxMemoryManager* ucx_mgr)
      : options_(options), proxy_(proxy), ucx_mgr_(ucx_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const override;

 private:
  const ClientOptions&  options_;
  const ProxyRpc&        proxy_;
  UcxMemoryManager*      ucx_mgr_;
};

}  // namespace us3_turbo::client
