#pragma once

#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/session/session_manager.h"

namespace us3_turbo::proxy {

/**
 * @brief 控制面服务：实现 GDS 单对象 PUT 的完整闭环
 *        （OpenSession → GdsPut[backend] → ReportGdsPut → CompleteUpload），
 *        其余 6 个 RPC 一律返回 "not implemented in proxy v1"。
 *
 * v1：发 session + ticket + data_endpoint，由 backend 回调 ReportGdsPut 标记完成，
 *     CompleteUpload 查询结果。不连 cuObjServer，写盘由 backend 负责。
 *
 * session 凭证/状态由 SessionManager 持有（引用，非拥有）。
 *
 * 线程安全：本类无状态（gateway_id_ / backend_endpoint_ 构造后只读），所有 RPC
 * handler 可被 brpc 并发调用；session 状态的并发安全由 SessionManager 的 mu_ 保证。
 */
class ProxyControlPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  ProxyControlPlaneService(std::string gateway_id,
                           std::string backend_endpoint,
                           SessionManager& session_mgr);

  // ---- 真实实现的 RPC ----
  void OpenSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::OpenSessionRequest* request,
      ::us3_turbo::proxy::OpenSessionResponse* response,
      google::protobuf::Closure* done) override;

  // backend → proxy 完成通知
  void ReportGdsPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::ReportGdsPutRequest* request,
      ::us3_turbo::proxy::ReportGdsPutResponse* response,
      google::protobuf::Closure* done) override;

  // client 查询 PUT 结果
  void CompleteUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::CompleteUploadRequest* request,
      ::us3_turbo::proxy::CompleteUploadResponse* response,
      google::protobuf::Closure* done) override;

  // ---- v1 占位：不实现 ----
  void HeadObject(google::protobuf::RpcController* cntl,
                  const ::us3_turbo::proxy::HeadObjectRequest* request,
                  ::us3_turbo::proxy::HeadObjectResponse* response,
                  google::protobuf::Closure* done) override;

  void GdsGet(google::protobuf::RpcController* cntl,
              const ::us3_turbo::proxy::GdsChunkRequest* request,
              ::us3_turbo::proxy::GdsChunkResponse* response,
              google::protobuf::Closure* done) override;

  void GdsPut(google::protobuf::RpcController* cntl,
              const ::us3_turbo::proxy::GdsChunkRequest* request,
              ::us3_turbo::proxy::GdsChunkResponse* response,
              google::protobuf::Closure* done) override;

  void AbortSession(google::protobuf::RpcController* cntl,
                    const ::us3_turbo::proxy::AbortSessionRequest* request,
                    ::us3_turbo::proxy::AbortSessionResponse* response,
                    google::protobuf::Closure* done) override;

  void StartUpload(google::protobuf::RpcController* cntl,
                   const ::us3_turbo::proxy::StartUploadRequest* request,
                   ::us3_turbo::proxy::StartUploadResponse* response,
                   google::protobuf::Closure* done) override;

  void AbortUpload(google::protobuf::RpcController* cntl,
                   const ::us3_turbo::proxy::AbortUploadRequest* request,
                   ::us3_turbo::proxy::AbortUploadResponse* response,
                   google::protobuf::Closure* done) override;

 private:
  std::string gateway_id_;
  std::string backend_endpoint_;  // 回填进 OpenSessionResponse.data_endpoint
  SessionManager& session_mgr_;
};

}  // namespace us3_turbo::proxy
