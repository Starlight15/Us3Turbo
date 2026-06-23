#pragma once

#include <chrono>
#include <string>

#include "client/src/common/rpc_base.h"
#include "client/src/contracts/requests.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief 数据面 RPC client(→ backend)。
 *
 * 按对端角色命名:本类封装 client → backend 的数据面 chunk RPC 能力,供后续
 * 数据传输操作在此扩充。ctor 接收数据面 endpoint + 超时,基类 RpcBase 内部
 * 就地 Init 一条 baidu_std channel 并建好 stub。
 *
 * 当前已落地:Put(GdsPut);一次 PUT 发一个 GdsChunk 请求,把 cuObject 颁发的
 * RDMA token 带上,backend 据此 RDMA-READ client 显存。
 * 其余数据面 RPC(GdsGet)等 backend 侧落地后在此扩充。
 *
 * 失败分类(is_data_plane=true)由基类 ctor 钉死:非超时失败归
 * kTransportError(而非控制面错误)。channel 构建与失败检查由 RpcBase 统一负责。
 */
class ChunkRpc : public RpcBase {
 public:
  /**
   * @param endpoint 数据面 endpoint "host:port"(必填)。
   * @param timeout  connect / RPC 超时,同时用于 channel 的
   *                 connect_timeout_ms 与 timeout_ms。
   * Init 失败时对象处于空态,ok() 返回 false。
   */
  ChunkRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "data", /*is_data_plane=*/true) {}

  [[nodiscard]] Result<us3_turbo::proxy::GdsChunkResponse>
    Put(const GdsChunkRequest& request) const;
};

}  // namespace us3_turbo::client
