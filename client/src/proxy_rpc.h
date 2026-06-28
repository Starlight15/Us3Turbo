#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "client/src/common/rpc_base.h"
#include "client/src/contracts/requests.h"

namespace us3_turbo::client {

/**
 * @brief proxy 控制面 RPC client（Mode B）。
 *
 * client 只与 proxy 交互：OpenSession / AbortSession / GdsPut 全部走同一条
 * brpc channel（指向 options.endpoint=proxy）。protobuf 响应类型只在
 * proxy_rpc.cpp 内部出现，对外只回填客户端自有的 SessionGrant / GdsPutResult。
 *
 * 单 channel 足够：brpc::Channel 线程安全、内部带连接复用，可被多线程
 * 并发调用（PutObject 重试 / bench 多 worker 共享同一个 Client）。
 */
class ProxyRpc : public RpcBase {
 public:
  ProxyRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "proxy") {}

  [[nodiscard]] bool OpenSession(const PutAttempt& attempt,
                                 SessionGrant& grant) const;

  /** best-effort：RPC 失败只 warn，不传播错误。 */
  void AbortSession(const std::string& session_id,
                    std::chrono::milliseconds timeout) const;

  [[nodiscard]] bool Put(const PutAttempt& attempt,
                         const SessionGrant& grant,
                         std::string_view rdma_token,
                         std::size_t chunk_size,
                         GdsPutResult& result) const;
};

}  // namespace us3_turbo::client
