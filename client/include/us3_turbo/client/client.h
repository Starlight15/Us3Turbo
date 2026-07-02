#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "client/src/common/put_request.h"
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
 * 调用方只设 request.path(kGds / kUcx),Client 选一条 PutChannel,链路细节
 * 藏在 channel 实现里。公共逻辑:校验 + retry-once(失败等 100ms 再试一次,
 * 共最多两次)。buffer 注册/注销在 channel 内部懒注册,对外无注册 API。
 */
class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** @brief 初始化 brpc 与 GDS/UCX channel,幂等。 */
  [[nodiscard]] bool Initialize();

  void Shutdown();

  [[nodiscard]] bool initialized() const;

  /**
   * @brief 统一 PUT 入口:按 request.path 选 GDS/UCX 通路。
   *
   * kGds→gds_channel_ 回 response.gds_result;kUcx→ucx_channel_ 回
   * response.ucx_result;kNone/kAll 拒绝。描述符由 channel 内部按 path 懒注册。
   * retry-once:首次失败等 100ms 再试一次,共最多两次,返回最终 result.ok。
   *
   * @return true 通路成功;false 失败或被拒绝。
   */
  [[nodiscard]] bool PutObject(const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& response) const;

 private:
  ClientOptions              options_;
  // 单 channel 指向 proxy,承载 GdsPut / UcxPut(两链路共享)。
  std::unique_ptr<ProxyRpc>  proxy_;
  // 路由落点:kGds→gds_channel_ ; kUcx→ucx_channel_。
  std::unique_ptr<GdsPutChannel> gds_channel_;
  std::unique_ptr<UcxPutChannel> ucx_channel_;
  bool                        initialized_{false};

  // path 校验:kNone / kAll 拒绝;source 由 channel 内填。
  [[nodiscard]] bool ValidatePutPath(const ClientProxyPutRequest& req) const;

  // 路由落点:返回对应 channel 或 nullptr。kAll 未来只在此扩展。
  [[nodiscard]] PutChannel* SelectChannel(PutDataPath path) const noexcept;
};

}  // namespace us3_turbo::client
