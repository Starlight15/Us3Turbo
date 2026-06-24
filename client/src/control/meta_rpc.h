#pragma once

#include <chrono>
#include <string>

#include "client/src/common/rpc_base.h"
#include "client/src/contracts/requests.h"

namespace us3_turbo::client {

/**
 * @brief 控制面 RPC client(→ proxy)。
 *
 * 当前已落地:OpenSession / AbortSession(单对象 GDS PUT 在用)。
 * channel 构建与失败检查由 RpcBase 统一负责。protobuf 响应类型只在
 * meta_rpc.cpp 内部出现,对外只回填客户端自有的 SessionGrant。
 */
class MetaRpc : public RpcBase {
 public:
  MetaRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "control") {}

  [[nodiscard]] bool OpenSession(const PutAttempt& attempt,
                                SessionGrant& grant) const;

  /** best-effort:RPC 失败只 warn,不传播错误。 */
  void AbortSession(const std::string& session_id,
                    std::chrono::milliseconds timeout) const;
};

}  // namespace us3_turbo::client
