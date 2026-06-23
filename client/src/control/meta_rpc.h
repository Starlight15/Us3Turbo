#pragma once

#include <chrono>
#include <string>

#include "client/src/common/errors.h"
#include "client/src/common/rpc_base.h"
#include "client/src/contracts/requests.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief 控制面 RPC client(→ proxy)。
 *
 * 按对端角色命名:本类封装 client → proxy 的控制面 RPC 能力,供后续元数据 /
 * 会话 / 分片等控制面操作在此扩充。ctor 接收控制面 endpoint + 超时,基类
 * RpcBase 内部就地 Init 一条 baidu_std channel 并建好 stub。
 *
 * 当前已落地:OpenSession / AbortSession(单对象 GDS PUT 在用)。
 * 其余控制面 RPC(HeadObject / StartUpload / CompleteUpload / AbortUpload)
 * 等 proxy 侧落地后在此扩充,方法形如现有 OpenSession。
 *
 * 失败分类(is_data_plane=false)由基类 ctor 钉死:非超时失败归
 * kControlPlaneError。channel 构建与失败检查由 RpcBase 统一负责。
 *
 * 注意:ReportGdsPut 是 backend → proxy 的完成回调,由 backend 持自己的 proxy
 * channel 调用,不属于 client → proxy 的控制面能力,故不在本类。
 */
class MetaRpc : public RpcBase {
 public:
  /**
   * @param endpoint 控制面 endpoint "host:port"(必填)。
   * @param timeout  connect / RPC 超时,同时用于 channel 的
   *                 connect_timeout_ms 与 timeout_ms。
   * Init 失败时对象处于空态,ok() 返回 false。
   */
  MetaRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "control", /*is_data_plane=*/false) {}

  [[nodiscard]] Result<us3_turbo::proxy::OpenSessionResponse>
    OpenSession(const OpenSessionRequest& request) const;

  /** best-effort:RPC 失败也算 success(false);不存在的 session 视为成功。 */
  [[nodiscard]] Result<bool> AbortSession(const std::string& session_id,
                                          std::chrono::milliseconds timeout) const;
};

}  // namespace us3_turbo::client
