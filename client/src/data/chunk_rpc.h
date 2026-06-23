#pragma once

#include <chrono>
#include <memory>

#include <brpc/channel.h>

#include "client/src/contracts/requests.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief 数据面 RPC client(→ backend)。
 *
 * 按对端角色命名:本类封装 client → backend 的数据面 chunk RPC 能力,供后续
 * 数据传输操作在此扩充。ctor 接收数据面 endpoint + 超时,内部就地 Init 一条
 * baidu_std channel 并建好 stub。
 *
 * 当前已落地:Put(GdsPut);一次 PUT 发一个 GdsChunk 请求,把 cuObject 颁发的
 * RDMA token 带上,backend 据此 RDMA-READ client 显存。
 * 其余数据面 RPC(GdsGet)等 backend 侧落地后在此扩充。
 *
 * 设计:ctor 内完成 channel 构建与 stub 绑定;channel 与 stub 同生共死,
 * 生命周期由所有权保证。Init 失败(channel 不可用)以 unique_ptr 空态表达,
 * 调用 Put 前应先调 ok() 判定。
 */
class ChunkRpc {
 public:
  /**
   * @param endpoint 数据面 endpoint "host:port"(必填)。
   * @param timeout  connect / RPC 超时,同时用于 channel 的
   *                 connect_timeout_ms 与 timeout_ms。
   * Init 失败时对象处于空态,ok() 返回 false。
   */
  ChunkRpc(const std::string& endpoint, std::chrono::milliseconds timeout);

  [[nodiscard]] Result<us3_turbo::proxy::GdsChunkResponse>
    Put(const GdsChunkRequest& request) const;

  /** channel 是否成功 Init。false 表示本对象不可用,任何 RPC 都会失败。 */
  [[nodiscard]] bool ok() const { return channel_ != nullptr && stub_ != nullptr; }

  /** Init 失败的原因(ok()==true 时为空)。 */
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

 private:
  std::unique_ptr<brpc::Channel>                                channel_;
  std::unique_ptr<us3_turbo::proxy::Control_Stub> stub_;
  std::string                                                    init_error_;
};

}  // namespace us3_turbo::client
