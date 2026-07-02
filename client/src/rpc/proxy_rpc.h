#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "control_plane.pb.h"
#include "client/src/common/request.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief proxy 控制面 RPC client:GdsPut / UcxPut 共用一条 brpc channel。
 */
class ProxyRpc {
 public:
  ProxyRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : default_timeout_(timeout) {
    if (endpoint.empty()) {
      init_error_ = "proxy endpoint must not be empty";
      return;
    }
    channel_ = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions co;
    co.protocol           = "baidu_std";
    co.connect_timeout_ms = static_cast<int>(timeout.count());
    co.timeout_ms         = static_cast<int>(timeout.count());
    co.max_retry          = 2;
    std::string trimmed = endpoint;
    while (!trimmed.empty() && trimmed.back() == '/') {
      trimmed.pop_back();
    }
    if (channel_->Init(trimmed.c_str(), nullptr, &co) != 0) {
      init_error_ = "Failed to initialize brpc channel: " + endpoint;
      channel_.reset();
      return;
    }
    stub_ = std::make_unique<us3_turbo::proxy::Control_Stub>(channel_.get());
  }

  ProxyRpc(const ProxyRpc&)            = delete;
  ProxyRpc& operator=(const ProxyRpc&) = delete;
  ProxyRpc(ProxyRpc&&)                 = delete;
  ProxyRpc& operator=(ProxyRpc&&)      = delete;

  ~ProxyRpc() = default;

  /** @brief RPC 层是否就绪(channel + stub 初始化成功)。 */
  [[nodiscard]] bool ok() const { return channel_ != nullptr && stub_ != nullptr; }

  /** @brief 初始化失败时的错误描述(ok() 为 true 时为空)。 */
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

  /** @brief GDS 通路:cuObj RDMA token 随 RPC 透传,backend 反向 RDMA-READ。 */
  [[nodiscard]] bool GdsPut(std::string_view request_id,
                            const std::string& bucket,
                            const std::string& key,
                            std::uint64_t object_size,
                            const GdsDataSource& gds_source,
                            PutPathResult& result) const;

  /** @brief UCX 通路:描述符随 RPC 透传,backend ucp_get_nbx 反向拉取。 */
  [[nodiscard]] bool UcxPut(std::string_view request_id,
                            const std::string& bucket,
                            const std::string& key,
                            std::uint64_t object_size,
                            const UcxDataSource& ucx_source,
                            PutPathResult& result) const;

 private:
  void ApplyTimeout(brpc::Controller& controller) const {
    controller.set_timeout_ms(static_cast<int>(default_timeout_.count()));
  }

  [[nodiscard]] us3_turbo::proxy::Control_Stub* stub() const { return stub_.get(); }

  // 默认 RPC 超时(用 options.default_timeout)。
  std::chrono::milliseconds                         default_timeout_{};
  std::unique_ptr<brpc::Channel>                    channel_;
  std::unique_ptr<us3_turbo::proxy::Control_Stub>   stub_;
  std::string                                        init_error_;
};

}  // namespace us3_turbo::client
