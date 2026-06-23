#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <brpc/channel.h>

#include "client/src/common/rpc_context.h"
#include "client/src/contracts/requests.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief 控制面 RPC client(→ proxy)。
 *
 * 按对端角色命名:本类封装 client → proxy 的控制面 RPC 能力,供后续元数据 /
 * 会话 / 分片等控制面操作在此扩充。ctor 接收控制面 endpoint + 超时,内部
 * 就地 Init 一条 baidu_std channel 并建好 stub。
 *
 * 当前已落地:OpenSession / AbortSession(单对象 GDS PUT 在用)。
 * 其余控制面 RPC(HeadObject / StartUpload / CompleteUpload / AbortUpload)
 * 等 proxy 侧落地后在此扩充,方法形如现有 OpenSession。
 *
 * 设计:ctor 内完成 channel 构建与 stub 绑定;channel 与 stub 同生共死,
 * 生命周期由所有权保证。Init 失败(channel 不可用)以 unique_ptr 空态表达,
 * 调用 OpenSession/AbortSession 前应先调 ok() 判定。
 *
 * 注意:ReportGdsPut 是 backend → proxy 的完成回调,由 backend 持自己的 proxy
 * channel 调用,不属于 client → proxy 的控制面能力,故不在本类。
 */
class MetaRpc {
 public:
  /**
   * @param endpoint 控制 face endpoint "host:port"(必填)。
   * @param timeout  connect / RPC 超时,同时用于 channel 的
   *                 connect_timeout_ms 与 timeout_ms。
   * Init 失败时对象处于空态,ok() 返回 false。
   */
  MetaRpc(const std::string& endpoint, std::chrono::milliseconds timeout);

  [[nodiscard]] Result<us3_turbo::proxy::OpenSessionResponse>
    OpenSession(const OpenSessionRequest& request) const;

  /** best-effort:RPC 失败也算 success(false);不存在的 session 视为成功。 */
  [[nodiscard]] Result<bool> AbortSession(const std::string& session_id,
                                          const RpcCallMetadata& context) const;

  /** channel 是否成功 Init。false 表示本对象不可用,任何 RPC 都会失败。 */
  [[nodiscard]] bool ok() const { return channel_ != nullptr && stub_ != nullptr; }

  /** Init 失败的原因(ok()==true 时为空)。 */
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

 private:
  std::unique_ptr<brpc::Channel>                          channel_;
  std::unique_ptr<us3_turbo::proxy::Control_Stub>         stub_;
  std::string                                              init_error_;
};

}  // namespace us3_turbo::client
