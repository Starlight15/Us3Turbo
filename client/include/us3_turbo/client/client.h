#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "client/src/common/request.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class ProxyRpc;
class GdsPutChannel;
class UcxPutChannel;
class PutChannel;

/**
 * @brief 对象存储 client。
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
   */
  [[nodiscard]] bool PutObject(const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& response) const;

 private:
  ClientOptions                  options_;
  std::unique_ptr<ProxyRpc>      proxy_;
  std::unique_ptr<GdsPutChannel> gds_channel_;
  std::unique_ptr<UcxPutChannel> ucx_channel_;
  bool                        initialized_{false};

  [[nodiscard]] bool ValidatePutPath(const ClientProxyPutRequest& req) const;

  [[nodiscard]] PutChannel* SelectChannel(PutDataPath path) const noexcept;
};

}  // namespace us3_turbo::client
