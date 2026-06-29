#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "client/src/common/rpc_base.h"
#include "client/src/contracts/requests.h"

namespace us3_turbo::client {

/**
 * @brief proxy 控制面 RPC client（Mode B）。
 *
 * client 只与 proxy 交互：GdsPut / RdmaPut 走同一条 brpc channel（指向
 * options.endpoint=proxy），单次 RPC 自带描述符。protobuf 响应类型只在
 * proxy_rpc.cpp 内部出现，对外只回填客户端自有的 GdsPutResult。
 *
 * 单 channel 足够：brpc::Channel 线程安全、内部带连接复用，可被多线程
 * 并发调用（PutObject 重试 / bench 多 worker 共享同一个 Client）。
 */
class ProxyRpc : public RpcBase {
 public:
  ProxyRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "proxy") {}

  [[nodiscard]] bool Put(const PutAttempt& attempt,
                         std::string_view rdma_token,
                         std::size_t chunk_size,
                         GdsPutResult& result) const;

  /**
   * @brief RDMA(UCX)链路的 RdmaPut：把 client 的 UCX 描述符（remote_addr /
   *        packed rkey / client_ucx_addr）随 RPC 透传给 proxy，proxy 转发
   *        backend，backend ucp_get_nbx 反向拉取。与 Gds 的 Put 完全独立。
   */
  [[nodiscard]] bool RdmaPut(const PutAttempt& attempt,
                             std::uint64_t remote_addr,
                             std::string_view rkey,
                             std::string_view client_ucx_addr,
                             std::size_t chunk_size,
                             GdsPutResult& result) const;
};

}  // namespace us3_turbo::client
