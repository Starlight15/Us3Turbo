#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <brpc/errno.pb.h>

#include "client/src/common/rpc_context.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/result.h"  // Error / MakeError / Result
#include "us3_turbo/client/types.h"   // ErrorCode / DataFlow / ToString

namespace us3_turbo::client {

/**
 * @brief MetaRpc / ChunkRpc 的公共基类:持有 brpc::Channel + Control_Stub,
 *        封装 channel 构建 / 超时应用 / 失败分类三件 brpc 胶水。
 *
 * 控制面(MetaRpc,→ proxy)与数据面(ChunkRpc,→ backend)共用同一条
 * baidu_std 通路结构,差异只在两点:角色名(role,用于空 endpoint 的报错前缀)
 * 与失败分类(非超时失败归 kControlPlaneError 还是 kTransportError)。两者由
 * ctor 的 @p is_data_plane 钉死,子类无需再关心。
 *
 * proto 只生成一个 Control_Stub(单一 service Control),控制面 / 数据面 RPC
 * 都是其方法,故 stub 由基类统一持有。
 *
 * Init 失败(channel 不可用)以 channel_/stub_ 空态表达,子类 RPC 入口先调
 * ok() 判定;基类 CheckFailure() 统一构造带 ErrorCode 的 Error。
 */
class RpcBase {
 public:
  /**
   * @param endpoint     对端 "host:port"(必填;空则 Init 失败)。
   * @param timeout      connect / RPC 超时,同时用于 channel 的
   *                     connect_timeout_ms 与 timeout_ms。
   * @param role         角色名("control" / "data"),仅用于空 endpoint 报错前缀。
   * @param is_data_plane true → 非超时失败归 kTransportError(数据面);
   *                      false → kControlPlaneError(控制面)。
   * Init 失败时对象处于空态,ok() 返回 false、init_error() 存原因。
   */
  RpcBase(const std::string& endpoint,
          std::chrono::milliseconds timeout,
          std::string_view role,
          bool is_data_plane)
      : is_data_plane_(is_data_plane) {
    if (endpoint.empty()) {
      init_error_ = role.empty()
                        ? std::string{"endpoint must not be empty"}
                        : std::string{role} + " endpoint must not be empty";
      return;
    }
    // 就地 Init 一条 baidu_std channel:connect / RPC 超时同源,max_retry=2。
    channel_ = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions co;
    co.protocol           = "baidu_std";
    co.connect_timeout_ms = static_cast<int>(timeout.count());
    co.timeout_ms         = static_cast<int>(timeout.count());
    co.max_retry          = 2;
    // 去尾斜杠,让 "host:port/" 与 "host:port" 等价。
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

  // 持有 channel + stub,不可拷贝 / 移动。
  RpcBase(const RpcBase&) = delete;
  RpcBase& operator=(const RpcBase&) = delete;
  RpcBase(RpcBase&&) = delete;
  RpcBase& operator=(RpcBase&&) = delete;

  virtual ~RpcBase() = default;

  /** channel + stub 是否就绪。false 表示本对象不可用,任何 RPC 都会失败。 */
  [[nodiscard]] bool ok() const { return channel_ != nullptr && stub_ != nullptr; }

  /** Init 失败的原因(ok()==true 时为空)。 */
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

 protected:
  /** 把 RpcCallMetadata.timeout 灌进 controller(目前只有 timeout)。 */
  void ApplyTimeout(brpc::Controller& controller, const RpcCallMetadata& context) const {
    controller.set_timeout_ms(static_cast<int>(context.timeout.count()));
  }

  /**
   * @brief 统一检查 brpc 调用是否失败,失败时构造带 ErrorCode 的 Error。
   *
   * brpc 超时 (ERPCTIMEDOUT / ETIMEDOUT) → kTimeout(retryable=true)。
   * 非超时失败:is_data_plane_ → kTransportError(数据面);否则
   * kControlPlaneError(控制面)。成功返回 Success(true)。
   */
  [[nodiscard]] Result<bool> CheckFailure(const brpc::Controller& controller,
                                          std::string_view message,
                                          std::string_view request_id) const {
    if (!controller.Failed()) {
      return Result<bool>::Success(true);
    }
    const int err = controller.ErrorCode();
    const bool is_timeout = (err == brpc::ERPCTIMEDOUT) || (err == ETIMEDOUT);
    const ErrorCode code = is_timeout
                               ? ErrorCode::kTimeout
                               : (is_data_plane_ ? ErrorCode::kTransportError
                                                 : ErrorCode::kControlPlaneError);
    return Result<bool>::Failure(MakeError(
        code,
        std::string(message) + ": " + controller.ErrorText(),
        /*retryable=*/true,
        std::string(ToString(DataFlow::GPUDirect)),
        std::string(request_id)));
  }

  /** Control_Stub 访问器(ok()==false 时为 nullptr)。 */
  [[nodiscard]] us3_turbo::proxy::Control_Stub* stub() const { return stub_.get(); }

 private:
  std::unique_ptr<brpc::Channel>                  channel_;
  std::unique_ptr<us3_turbo::proxy::Control_Stub> stub_;
  std::string                                      init_error_;
  bool                                             is_data_plane_;
};

}  // namespace us3_turbo::client
