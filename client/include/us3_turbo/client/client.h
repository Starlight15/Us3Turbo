#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "client/src/contracts/put_request.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class ProxyRpc;
class GdsPutChannel;
class UcxPutChannel;
class PutChannel;
struct ClientProxyPutRequest;
struct ClientProxyPutResponse;

/**
 * @brief 对象存储 client(Mode B 薄路由层)。
 *
 * 调用方只设 @ref ClientProxyPutRequest.path(kGds / kUcx,未来 kAll),
 * Client 按 path 选一条 PutChannel,链路细节(GDS device token / UCX host
 * 描述符)藏在各自 channel 实现里。Client 只保留公共逻辑:校验 +
 * retry-once(失败后等 100ms 再试一次,共最多两次)。两条链路的 buffer
 * 注册/注销全部在各 channel 内部懒注册,对外不暴露任何注册 API——
 * GDS 与 UCX 调用方写法完全对称。
 */
class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** @brief 初始化 brpc channel 与 GDS/UCX channel，幂等。返回 true 成功，false 失败。 */
  [[nodiscard]] bool Initialize();

  void Shutdown();

  [[nodiscard]] bool initialized() const;

  /**
   * @brief 统一 PUT 入口：按 @p request.path 选择 GDS/UCX 通路。
   *
   * path=kGds → gds_channel_,结果回 response.gds_result。
   * path=kUcx → ucx_channel_,结果回 response.ucx_result。
   * path=kNone / kAll → 拒绝（kAll 推迟：单 buffer 无法同时喂 device+host）。
   * 描述符由各 channel 内部按 path 从 buffer 获取（含首次懒注册），调用方
   * 不预填 source、不显式注册。
   *
   * retry-once：首次失败则等 100ms 再试一次，共最多两次调用；第二次结果
   * 无论成败都接受。@return 最终一次尝试的 result.ok。
   *
   * @return true 选中通路成功，false 失败或被拒绝。
   */
  [[nodiscard]] bool PutObject(const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& response) const;

 private:
  ClientOptions              options_;
  // Mode B：单 channel 指向 proxy，承载 GdsPut / UcxPut(两链路共享)。
  std::unique_ptr<ProxyRpc>  proxy_;
  // 模式路由的落点:kGds→gds_channel_ ; kUcx→ucx_channel_。各 channel 持有
  // 自己的内存管理器引用,Client 不再直接持有 gds_mgr_/ucx_mgr_。
  std::unique_ptr<GdsPutChannel> gds_channel_;
  std::unique_ptr<UcxPutChannel> ucx_channel_;
  bool                        initialized_{false};

  // path 校验：kNone 拒绝、kAll 拒绝（推迟）。source 不在此检查（channel 内填）。
  [[nodiscard]] bool ValidatePutPath(const ClientProxyPutRequest& req) const;

  // 模式路由的唯一落点:kGds→gds_channel_.get()、kUcx→ucx_channel_.get()、
  // 其余返回 nullptr。kAll 未来只在这一处扩展为"依次驱动两条链路"。
  [[nodiscard]] PutChannel* SelectChannel(PutDataPath path) const noexcept;
};

}  // namespace us3_turbo::client
