#include "proxy/src/service/proxy_control_plane_service.h"

#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

ProxyControlPlaneService::ProxyControlPlaneService(
    std::string gateway_id,
    std::string backend_endpoint,
    int backend_timeout_ms)
    : gateway_id_(std::move(gateway_id)),
      backend_endpoint_(std::move(backend_endpoint)),
      backend_timeout_ms_(backend_timeout_ms) {
  if (backend_endpoint_.empty()) {
    spdlog::warn("proxy: backend_endpoint empty, GdsPut will reject as "
                 "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }
  auto channel = std::make_shared<brpc::Channel>();
  brpc::ChannelOptions options;
  options.timeout_ms = backend_timeout_ms_;
  options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
  if (channel->Init(backend_endpoint_.c_str(), nullptr, &options) != 0) {
    spdlog::warn("proxy: failed to init backend channel to {}, GdsPut disabled",
                 backend_endpoint_);
    return;
  }
  backend_channel_ = std::move(channel);
  backend_stub_ = std::make_unique<::us3_turbo::proxy::Control_Stub>(
      backend_channel_.get());
  spdlog::info("proxy: backend forward channel ready at {} (timeout {}ms)",
               backend_endpoint_, backend_timeout_ms_);
}

void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* request,
    ::us3_turbo::proxy::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // 内联参数校验（原 OpenSession 职责塌入此处）。
  if (request->bucket().empty() || request->object_key().empty()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "missing bucket or object_key");
    return;
  }
  if (request->chunk_size() == 0) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "chunk_size must be > 0 for GDS PUT");
    return;
  }
  if (backend_stub_ == nullptr) {
    cntl->SetFailed(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel");
    return;
  }

  // ---- 同步转发给 backend（bthread 阻塞，会 yield 让出）----
  brpc::Controller bcntl;
  bcntl.set_timeout_ms(backend_timeout_ms_);
  ::us3_turbo::proxy::GdsChunkResponse bresp;
  backend_stub_->GdsPut(&bcntl, request, &bresp, nullptr);
  if (bcntl.Failed()) {
    cntl->SetFailed(PROXY_ERR_BACKEND_RPC,
                    "backend GdsPut failed: %s",
                    bcntl.ErrorText().c_str());
    return;
  }

  response->set_etag(bresp.etag());
  response->set_crc32c(bresp.crc32c());
  response->set_bytes_received(bresp.bytes_received());

  spdlog::info("GdsPut: forwarded etag={} crc32c={:x} bytes={}",
               bresp.etag(), bresp.crc32c(), bresp.bytes_received());
}

// ---------------------------------------------------------------------------
// RdmaPut（UCX 链路）：同步转发给 backend。与 GdsPut 代码独立、无共享逻辑，
// 仅因 brpc 一个 proto service 只能注册一个 C++ 实例而共处本类。
// ---------------------------------------------------------------------------
void ProxyControlPlaneService::RdmaPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::RdmaChunkRequest* request,
    ::us3_turbo::proxy::RdmaChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  if (request->bucket().empty() || request->object_key().empty()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "missing bucket or object_key");
    return;
  }
  if (request->chunk_size() == 0) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "chunk_size must be > 0 for RDMA PUT");
    return;
  }
  if (backend_stub_ == nullptr) {
    cntl->SetFailed(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel");
    return;
  }

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(backend_timeout_ms_);
  ::us3_turbo::proxy::RdmaChunkResponse bresp;
  backend_stub_->RdmaPut(&bcntl, request, &bresp, nullptr);
  if (bcntl.Failed()) {
    cntl->SetFailed(PROXY_ERR_BACKEND_RPC,
                    "backend RdmaPut failed: %s",
                    bcntl.ErrorText().c_str());
    return;
  }

  response->set_etag(bresp.etag());
  response->set_crc32c(bresp.crc32c());
  response->set_bytes_received(bresp.bytes_received());

  spdlog::info("RdmaPut: forwarded etag={} crc32c={:x} bytes={}",
               bresp.etag(), bresp.crc32c(), bresp.bytes_received());
}

}  // namespace us3_turbo::proxy
