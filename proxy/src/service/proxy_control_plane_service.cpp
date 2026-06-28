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
    int backend_timeout_ms,
    SessionManager& session_mgr)
    : gateway_id_(std::move(gateway_id)),
      backend_endpoint_(std::move(backend_endpoint)),
      backend_timeout_ms_(backend_timeout_ms),
      session_mgr_(session_mgr) {
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

void ProxyControlPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::OpenSessionRequest* request,
    ::us3_turbo::proxy::OpenSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // GDS PUT 必填：bucket / object_key / expected_size。
  if (request->bucket().empty() || request->object_key().empty()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "missing bucket or object_key");
    return;
  }
  if (request->expected_size() == 0) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "expected_size must be > 0 for GDS PUT");
    return;
  }

  // ---- 生成 session 凭证（委托给 SessionManager）----
  const auto session = session_mgr_.CreateSession(
      request->session_id(), request->bucket(), request->object_key(),
      request->expected_size());

  // ---- 填充响应（Mode B：不下发 data_endpoint）----
  response->set_request_id(request->request_id());
  response->set_session_id(session.session_id);
  response->set_ticket(session.ticket);

  spdlog::info("OpenSession: session_id={}, ticket={}, bucket={}, key={}, "
               "size={}",
               session.session_id, session.ticket, request->bucket(),
               request->object_key(), request->expected_size());
}

void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* request,
    ::us3_turbo::proxy::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // 校验 session 存在（仅存在性判空，不持有返回指针跨 RPC）。
  if (session_mgr_.GetSession(request->session_id()) == nullptr) {
    cntl->SetFailed(PROXY_ERR_SESSION_NOT_FOUND,
                    "unknown or expired session_id=%s",
                    request->session_id().c_str());
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

  // ---- 索引提交钩子：backend 传完、proxy 落索引后再回 client ----
  const bool committed = session_mgr_.CompleteSession(
      request->session_id(), bresp.etag(), bresp.crc32c(),
      bresp.bytes_received());
  if (!committed) {
    spdlog::warn("GdsPut: CompleteSession failed (session gone?) session_id={}",
                 request->session_id());
    // 仍返回数据：backend 已传完，对 client 报失败会触发无谓重传。
  }

  response->set_etag(bresp.etag());
  response->set_crc32c(bresp.crc32c());
  response->set_bytes_received(bresp.bytes_received());

  spdlog::info("GdsPut: forwarded session_id={} etag={} crc32c={:x} bytes={}",
               request->session_id(), bresp.etag(), bresp.crc32c(),
               bresp.bytes_received());
}

// v1 占位：proxy 不直接处理 AbortSession 语义（client 失败路径 best-effort
// 调用并忽略失败）。返回 ENOMETHOD 与历史行为一致。
void ProxyControlPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortSessionRequest* /*request*/,
    ::us3_turbo::proxy::AbortSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(PROXY_ERR_NOT_IMPLEMENTED, "%s",
                  std::string(ErrorMessage(PROXY_ERR_NOT_IMPLEMENTED)).c_str());
}

}  // namespace us3_turbo::proxy
