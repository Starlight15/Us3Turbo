#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "backend/src/backend_gds_sink.h"
#include "control_plane.pb.h"

namespace us3_turbo::backend {

/**
 * @brief 数据面服务：仅真实实现 GdsPut 的单对象分支（upload_id 为空），
 *        用客户端 token 反向 RDMA-READ 拉字节后丢弃。
 *
 * 职责：参数校验 → 调 sink_.ReceiveAndDiscard() → 返回 etag + bytes_received
 *       →（可选）异步通知 proxy 数据传输完成。
 * 不做：session/ticket 校验（proxy 是另一进程，已校验），写盘（v1 丢弃）。
 *
 * 其余 7 个 RPC 与“GdsPut 收到非空 upload_id”一律
 * cntl->SetFailed(brpc::ENOMETHOD, "backend v1: only single-object GdsPut")。
 *
 * 线程安全：本类无状态（gateway_id_ 构造后只读），所有 RPC handler 可被
 * brpc 并发调用；sink_ 在 BackendGdsSink::Start() 后恒定不变，故 GdsPut 可
 * 无锁并发（RDMA 资源的并发访问由 sink_ 内部保证）。proxy_channel_ /
 * proxy_stub_ 构造后恒定不变，亦可无锁并发访问。
 *
 * 依赖：
 * - BackendGdsSink（构造注入引用，不拥有所有权，生命周期由 main.cpp 管理）。
 * - proxy_endpoint 非空时构造 brpc::Channel + Control_Stub，
 *   用于 GdsPut 完成后通知 proxy；空串=不通知（单进程测试场景）。
 */
class BackendDataPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  // proxy_endpoint 空串表示不通知 proxy。
  BackendDataPlaneService(BackendGdsSink& sink,
                          std::string gateway_id,
                          const std::string& proxy_endpoint);

  void OpenSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::OpenSessionRequest* request,
      ::us3_turbo::proxy::OpenSessionResponse* response,
      google::protobuf::Closure* done) override;

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

  void CompleteUpload(google::protobuf::RpcController* cntl,
                      const ::us3_turbo::proxy::CompleteUploadRequest* request,
                      ::us3_turbo::proxy::CompleteUploadResponse* response,
                      google::protobuf::Closure* done) override;

  void AbortUpload(google::protobuf::RpcController* cntl,
                   const ::us3_turbo::proxy::AbortUploadRequest* request,
                   ::us3_turbo::proxy::AbortUploadResponse* response,
                   google::protobuf::Closure* done) override;

 private:
  BackendGdsSink& sink_;
  std::string     gateway_id_;

  // proxy_endpoint 非空时构造，GdsPut 完成后用于异步通知 proxy。
  std::shared_ptr<brpc::Channel>                      proxy_channel_;
  std::unique_ptr<::us3_turbo::proxy::Control_Stub>
      proxy_stub_;
};

}  // namespace us3_turbo::backend
