#pragma once

#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "backend/src/backend_gds_sink.h"
#include "backend/src/rdma/ucx_sink.h"
#include "control_plane.pb.h"

namespace us3_turbo::backend {

/**
 * @brief 纯数据面服务（Mode B）：只被 proxy 调用。
 *
 * 职责：proxy 转发的 GdsPut → 参数校验 → sink_.ReceiveAndDiscard()（反向
 *   RDMA-READ 拉 client 显存到 pinned buffer 后丢弃）→ 返回 PutPathResult
 *   （ok/etag/crc32c/bytes_written）给 proxy。proxy 无状态转发，backend
 *   不再反向通知 proxy。
 *
 * 本类同时实现 UcxPut：因 brpc 一个 proto service 只能注册一个 C++ 服务
 * 实例（按 service descriptor full_name 去重），gds 与 ucx 两条链路在
 * backend 同一进程内无法各注册一个 Control 子类。故 UcxPut 也由本类持有，
 * 内部委托给 ucx_sink_。与"两链路不抽象"原则的折中：UcxPut 与 GdsPut
 * 代码完全独立、无共享逻辑，仅在 brpc 注册层面共处一个 service 对象。
 *
 * 不做：写盘（v1 丢弃）。
 *
 * 线程安全：本类无状态，所有 RPC handler 可被 brpc 并发调用；sink_ 在
 * BackendGdsSink::Start() 后恒定不变，故 GdsPut 可无锁并发（RDMA 资源的
 * 并发访问由 sink_ 内部保证）。
 *
 * 依赖：BackendGdsSink（构造注入引用，不拥有所有权，生命周期由 main.cpp 管理）。
 *       UcxSink（ucx 链路，构造注入引用）。
 */
class BackendDataPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  BackendDataPlaneService(BackendGdsSink& sink, rdma::UcxSink& ucx_sink);

  void GdsPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::ClientProxyPutRequest* request,
      ::us3_turbo::proxy::PutPathResult* response,
      google::protobuf::Closure* done) override;

  void UcxPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::ClientProxyPutRequest* request,
      ::us3_turbo::proxy::PutPathResult* response,
      google::protobuf::Closure* done) override;

 private:
  BackendGdsSink&      sink_;
  rdma::UcxSink&       ucx_sink_;
};

}  // namespace us3_turbo::backend
