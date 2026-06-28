#pragma once

#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "backend/src/backend_gds_sink.h"
#include "control_plane.pb.h"

namespace us3_turbo::backend {

/**
 * @brief 纯数据面服务（Mode B）：只被 proxy 调用。
 *
 * 职责：proxy 转发的 GdsPut → 参数校验 → sink_.ReceiveAndDiscard()（反向
 *   RDMA-READ 拉 client 显存到 pinned buffer 后丢弃）→ 返回 etag +
 *   bytes_received + crc32c 给 proxy。proxy 拿到响应后自己做索引提交
 *   （CompleteSession），backend 不再反向通知 proxy。
 *
 * 不做：session/ticket 校验（proxy 已校验），写盘（v1 丢弃）。
 *
 * 其余 2 个 RPC（OpenSession / AbortSession）一律
 * cntl->SetFailed(brpc::ENOMETHOD, "backend v1: only single-object GdsPut")。
 *
 * 线程安全：本类无状态，所有 RPC handler 可被 brpc 并发调用；sink_ 在
 * BackendGdsSink::Start() 后恒定不变，故 GdsPut 可无锁并发（RDMA 资源的
 * 并发访问由 sink_ 内部保证）。
 *
 * 依赖：BackendGdsSink（构造注入引用，不拥有所有权，生命周期由 main.cpp 管理）。
 */
class BackendDataPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  explicit BackendDataPlaneService(BackendGdsSink& sink);

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
  BackendGdsSink& sink_;
};

}  // namespace us3_turbo::backend
