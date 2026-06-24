#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "control_plane.pb.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief MetaRpc / ChunkRpc 的公共基类：持有 brpc::Channel + Control_Stub。
 *
 * 控制面(MetaRpc,→ proxy)与数据面(ChunkRpc,→ backend)共用同一条
 * baidu_std 通路结构，差异只在 @p role 名（用于 init_error 文案）。
 */
class RpcBase {
 public:
  RpcBase(const std::string& endpoint,
          std::chrono::milliseconds timeout,
          std::string_view role) {
    if (endpoint.empty()) {
      init_error_ = role.empty()
                        ? std::string{"endpoint must not be empty"}
                        : std::string{role} + " endpoint must not be empty";
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

  RpcBase(const RpcBase&) = delete;
  RpcBase& operator=(const RpcBase&) = delete;
  RpcBase(RpcBase&&) = delete;
  RpcBase& operator=(RpcBase&&) = delete;

  virtual ~RpcBase() = default;

  [[nodiscard]] bool ok() const { return channel_ != nullptr && stub_ != nullptr; }
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

 protected:
  void ApplyTimeout(brpc::Controller& controller, std::chrono::milliseconds timeout) const {
    controller.set_timeout_ms(static_cast<int>(timeout.count()));
  }

  [[nodiscard]] us3_turbo::proxy::Control_Stub* stub() const { return stub_.get(); }

 private:
  std::unique_ptr<brpc::Channel>                  channel_;
  std::unique_ptr<us3_turbo::proxy::Control_Stub> stub_;
  std::string                                      init_error_;
};

}  // namespace us3_turbo::client
