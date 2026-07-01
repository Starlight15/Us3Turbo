#include "proxy/src/service/proxy_control_plane_service.h"

#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

namespace {

// path 是否包含某通路（bitflags：PATH_ALL=PATH_GDS|PATH_UCX）。
bool HasPath(::us3_turbo::proxy::PutDataPath flags,
             ::us3_turbo::proxy::PutDataPath check) {
  return (static_cast<int>(flags) & static_cast<int>(check)) != 0;
}

}  // namespace

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

// 把 backend 返回的 PutPathResult 原样回填给 client 响应。backend 已填好
// ok/etag/crc32c/bytes_written（失败时 ok=false + error_*）。
static void ForwardResult(::us3_turbo::proxy::PutPathResult* response,
                          const ::us3_turbo::proxy::PutPathResult& bresp) {
  response->CopyFrom(bresp);
}

void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // 内联参数校验。
  if (request->bucket().empty() || request->key().empty()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "missing bucket or key");
    return;
  }
  if (request->object_size() == 0) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "object_size must be > 0 for GDS PUT");
    return;
  }
  if (!HasPath(request->path(), ::us3_turbo::proxy::PATH_GDS)) {
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED,
                    "GdsPut requires path with PATH_GDS");
    return;
  }
  if (!request->has_gds_source()) {
    cntl->SetFailed(PROXY_ERR_MISSING_SOURCE,
                    "GdsPut requires gds_source");
    return;
  }
  if (backend_stub_ == nullptr) {
    cntl->SetFailed(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel");
    return;
  }

  // ---- 同步转发给 backend（bthread 阻塞，会 yield 让出）----
  brpc::Controller bcntl;
  bcntl.set_timeout_ms(backend_timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  backend_stub_->GdsPut(&bcntl, request, &bresp, nullptr);
  if (bcntl.Failed()) {
    cntl->SetFailed(PROXY_ERR_BACKEND_RPC,
                    "backend GdsPut failed: %s",
                    bcntl.ErrorText().c_str());
    return;
  }

  ForwardResult(response, bresp);

  spdlog::info("GdsPut: forwarded etag={} crc32c={:x} bytes={}",
               bresp.etag(), bresp.crc32c(), bresp.bytes_written());
}

// ---------------------------------------------------------------------------
// UcxPut（UCX 链路）：同步转发给 backend。与 GdsPut 代码独立、无共享逻辑，
// 仅因 brpc 一个 proto service 只能注册一个 C++ 实例而共处本类。
// ---------------------------------------------------------------------------
void ProxyControlPlaneService::UcxPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  if (request->bucket().empty() || request->key().empty()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "missing bucket or key");
    return;
  }
  if (request->object_size() == 0) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "object_size must be > 0 for UCX PUT");
    return;
  }
  if (!HasPath(request->path(), ::us3_turbo::proxy::PATH_UCX)) {
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED,
                    "UcxPut requires path with PATH_UCX");
    return;
  }
  if (!request->has_ucx_source()) {
    cntl->SetFailed(PROXY_ERR_MISSING_SOURCE,
                    "UcxPut requires ucx_source");
    return;
  }
  if (backend_stub_ == nullptr) {
    cntl->SetFailed(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel");
    return;
  }

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(backend_timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  backend_stub_->UcxPut(&bcntl, request, &bresp, nullptr);
  if (bcntl.Failed()) {
    cntl->SetFailed(PROXY_ERR_BACKEND_RPC,
                    "backend UcxPut failed: %s",
                    bcntl.ErrorText().c_str());
    return;
  }

  ForwardResult(response, bresp);

  spdlog::info("UcxPut: forwarded etag={} crc32c={:x} bytes={}",
               bresp.etag(), bresp.crc32c(), bresp.bytes_written());
}

}  // namespace us3_turbo::proxy
