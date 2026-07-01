#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class ProxyRpc;
class GdsMemoryManager;
class UcxMemoryManager;
struct ClientProxyPutRequest;
struct ClientProxyPutResponse;

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
   * @brief 统一 PUT 入口：按 @p request.path 选择 GDS/UCX 通路。
   *
   * path=kGds → AcquireToken(device) → GdsPut，结果回 response.gds_result。
   * path=kUcx → AcquireDescriptor(host) → UcxPut，结果回 response.ucx_result。
   * path=kNone / kAll → 拒绝（kAll 推迟：单 buffer 无法同时喂 device+host）。
   * 描述符由内部按 path 从 buffer 获取，调用方不预填 source。
   *
   * @return true 选中通路成功，false 失败或被拒绝。
   */
  [[nodiscard]] bool PutObject(const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& response) const;

  [[nodiscard]] bool RegisterDeviceBuffer(void* ptr, std::size_t size);
  [[nodiscard]] bool UnregisterDeviceBuffer(void* ptr);

 private:
  ClientOptions              options_;
  // Mode B：单 channel 指向 proxy，承载 GdsPut / UcxPut。
  std::unique_ptr<ProxyRpc>  proxy_;
  GdsMemoryManager*          gds_mgr_{nullptr};
  UcxMemoryManager*          ucx_mgr_{nullptr};
  bool                       initialized_{false};

  // path 校验：kNone 拒绝、kAll 拒绝（推迟）。source 不在此检查（内部填）。
  [[nodiscard]] bool ValidatePutPath(const ClientProxyPutRequest& req) const;

  // 公共重试模板：deadline 截止则放弃，否则交给 ExecuteWithRetry 重试。
  // method_name 用于 deadline 超时日志。模板只在 client.cpp 实例化
  //（PutObject 的 GDS/UCX 分支），定义置于 .cpp（唯一编译单元，不违反 ODR）。
  template <typename PutFunc>
  [[nodiscard]] bool ExecutePutWithRetry(const ClientProxyPutRequest& request,
                                         std::string_view method_name,
                                         PutFunc&& put_operation) const;
};

}  // namespace us3_turbo::client
