#pragma once

// gds_put_channel.h — GDS(CUObj RDMA)链路的 PutChannel 实现。
//
// 与 ucx_put_channel.* 物理隔离(互不 include),可单独回归——GDS 机器稀缺
// 时 GDS 链路不能被 UCX 依赖拖累(见 review/client_refactor_prompt.md 硬约束)。
//
// 持有 ClientOptions / ProxyRpc / GdsMemoryManager 的引用/指针(均由 Client
// 在 Initialize 时传入并保活),PutOnce 搬运原 client.cpp 的 GdsPutOnce 逻辑:
// AcquireToken(device) → 构 GdsDataSource → proxy.GdsPut → 回填 result →
// 可选 VerifyGdsCrc32c(含 D2H 拷贝)→ 可选 GDS trace。

#include <cstddef>

#include "client/src/gds_transport/gds_memory_manager.h"
#include "client/src/transport/put_channel.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

class ProxyRpc;

/**
 * @brief GDS 链路的 PutChannel。device 显存走 cuObj RDMA token + backend
 *        反向 RDMA-READ。构造持有的引用必须由 Client 保活到本对象销毁。
 */
class GdsPutChannel final : public PutChannel {
 public:
  GdsPutChannel(const ClientOptions& options, const ProxyRpc& proxy,
                GdsMemoryManager* gds_mgr)
      : options_(options), proxy_(proxy), gds_mgr_(gds_mgr) {}

  [[nodiscard]] bool PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const override;

  // GDS 专属:显式注册/注销 device buffer(pin 入 BAR1)。原 Client 公开 API
  // 归属到本链路(见 review 阶段4 方案A),通用 Client 不再暴露 GDS 痕迹。
  [[nodiscard]] bool RegisterDeviceBuffer(void* ptr, std::size_t size);
  [[nodiscard]] bool UnregisterDeviceBuffer(void* ptr);

 private:
  const ClientOptions&  options_;
  const ProxyRpc&        proxy_;
  GdsMemoryManager*      gds_mgr_;
};

}  // namespace us3_turbo::client
