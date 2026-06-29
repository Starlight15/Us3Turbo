#pragma once

#include <cstddef>
#include <memory>

#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class ProxyRpc;
class GdsMemoryManager;
class UcxMemoryManager;

class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** @brief 初始化 brpc channel 与 GDS/UCX 组件，幂等。返回 true 成功，false 失败。 */
  [[nodiscard]] bool Initialize();

  void Shutdown();

  [[nodiscard]] bool initialized() const;

  /**
   * @brief 通过 GDS 通路上传 @p buffer（device 显存）。返回 true 成功，false 失败。
   * @param out 成功时填充 TransferOutcome。
   */
  [[nodiscard]] bool PutObject(const PutObjectRequest& request,
                              ConstBufferView buffer,
                              TransferOutcome& out) const;

  /**
   * @brief 通过 RDMA(UCX) 通路上传 @p buffer（host 内存）。与 PutObject(gds)
   *        完全独立。返回 true 成功，false 失败。
   */
  [[nodiscard]] bool PutObjectRdma(const PutObjectRequest& request,
                                   ConstBufferView buffer,
                                   TransferOutcome& out) const;

  [[nodiscard]] bool RegisterDeviceBuffer(void* ptr, std::size_t size);
  [[nodiscard]] bool UnregisterDeviceBuffer(void* ptr);

 private:
  ClientOptions              options_;
  // Mode B：单 channel 指向 proxy，承载 GdsPut / RdmaPut。
  std::unique_ptr<ProxyRpc>  proxy_;
  GdsMemoryManager*          gds_mgr_{nullptr};
  UcxMemoryManager*          ucx_mgr_{nullptr};
  bool                       initialized_{false};
};

}  // namespace us3_turbo::client
