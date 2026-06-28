#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/session/session_manager.h"

namespace us3_turbo::proxy {

/**
 * @brief 控制面服务（Mode B）：client 只与本服务交互。
 *
 * 链路：client → OpenSession(拿 ticket) → GdsPut(带 rdma_token)；
 *   GdsPut 同步转发给 backend RDMA-READ，backend 返回 etag/crc/bytes 后，
 *   proxy 先 CompleteSession（索引提交钩子）再回 client 成功，确保闭环。
 *
 * backend 不再暴露给 client：OpenSession 不下发 data_endpoint。
 *
 * session 凭证/状态由 SessionManager 持有（引用，非拥有）。
 *
 * 线程安全：本类无状态（gateway_id_/backend_endpoint_ 构造后只读），
 * backend_channel_/backend_stub_ 构造后恒定不变，所有 RPC handler 可被
 * brpc 并发调用；session 状态的并发安全由 SessionManager 的 mu_ 保证。
 */
class ProxyControlPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  ProxyControlPlaneService(std::string gateway_id,
                           std::string backend_endpoint,
                           int backend_timeout_ms,
                           SessionManager& session_mgr);

  void OpenSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::OpenSessionRequest* request,
      ::us3_turbo::proxy::OpenSessionResponse* response,
      google::protobuf::Closure* done) override;

  void GdsPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::GdsChunkRequest* request,
      ::us3_turbo::proxy::GdsChunkResponse* response,
      google::protobuf::Closure* done) override;

  void AbortSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::AbortSessionRequest* request,
      ::us3_turbo::proxy::AbortSessionResponse* response,
      google::protobuf::Closure* done) override;

 private:
  std::string gateway_id_;
  std::string backend_endpoint_;  // backend 数据面地址，用于建 brpc channel
  int         backend_timeout_ms_;

  // 到 backend 的同步转发 channel（Mode B）。构造失败则 stub 为空，
  // GdsPut 以 PROXY_ERR_BACKEND_UNAVAILABLE 拒绝。
  std::shared_ptr<brpc::Channel>                     backend_channel_;
  std::unique_ptr<::us3_turbo::proxy::Control_Stub>   backend_stub_;
  SessionManager&                                     session_mgr_;
};

}  // namespace us3_turbo::proxy
