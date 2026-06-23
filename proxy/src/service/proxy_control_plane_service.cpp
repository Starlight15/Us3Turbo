#include "proxy/src/service/proxy_control_plane_service.h"

#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

namespace {

// GDS 通道标识，与 client DataFlow::GPUDirect → ToString("gds-cuobject") 一致。
constexpr std::string_view kGdsDataPath = "gds-cuobject";
constexpr std::string_view kOpTypePut   = "PUT";

}  // namespace

ProxyControlPlaneService::ProxyControlPlaneService(
    std::string gateway_id,
    std::string backend_endpoint,
    SessionManager& session_mgr)
    : gateway_id_(std::move(gateway_id)),
      backend_endpoint_(std::move(backend_endpoint)),
      session_mgr_(session_mgr) {}

void ProxyControlPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::OpenSessionRequest* request,
    ::us3_turbo::proxy::OpenSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // ---- 校验 ----
  if (request->data_flow() != kGdsDataPath) {
    cntl->SetFailed(PROXY_ERR_UNSUPPORTED_PATH,
                    "proxy v1 only accepts data_flow=gds-cuobject, got: %s",
                    request->data_flow().c_str());
    return;
  }
  if (request->op_type() != kOpTypePut) {
    cntl->SetFailed(PROXY_ERR_UNSUPPORTED_PATH,
                    "proxy v1 only accepts op_type=PUT, got: %s",
                    request->op_type().c_str());
    return;
  }
  if (request->is_multipart_part()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "proxy v1 does not support multipart part sessions");
    return;
  }
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

  // ---- 填充响应 ----
  response->set_request_id(request->request_id());
  response->set_session_id(session.session_id);
  response->set_ticket(session.ticket);
  response->set_gateway_id(gateway_id_);
  response->set_data_endpoint(backend_endpoint_);
  response->set_expire_at(session.expire_at);

  spdlog::info("OpenSession: session_id={}, ticket={}, bucket={}, key={}, "
               "size={}, data_endpoint={}",
               session.session_id, session.ticket, request->bucket(),
               request->object_key(), request->expected_size(),
               backend_endpoint_);
}

void ProxyControlPlaneService::ReportGdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ReportGdsPutRequest* request,
    ::us3_turbo::proxy::ReportGdsPutResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  static_cast<void>(cntl_base);

  const bool ok = session_mgr_.CompleteSession(
      request->session_id(), request->etag(), request->crc32c(),
      request->bytes_transferred());
  response->set_accepted(ok);
  if (ok) {
    spdlog::info("ReportGdsPut: session_id={} etag={} crc32c={:x} bytes={}",
                 request->session_id(), request->etag(),
                 request->crc32c(), request->bytes_transferred());
  } else {
    spdlog::warn("ReportGdsPut: unknown or expired session_id={}",
                 request->session_id());
  }
}

void ProxyControlPlaneService::CompleteUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::CompleteUploadRequest* request,
    ::us3_turbo::proxy::CompleteUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // upload_id 字段复用传 ticket：单对象 PUT 无独立 upload_id，
  // client 用 OpenSession 拿到的 ticket 作为完成查询凭证。
  const auto* session = session_mgr_.GetCompletedSession(request->upload_id());
  if (session == nullptr) {
    cntl->SetFailed(PROXY_ERR_SESSION_NOT_FOUND,
                    "session not found or not yet completed");
    return;
  }
  response->set_etag(session->etag);
}

// ---------------------------------------------------------------------------
// v1 占位方法：一律返回 "not implemented in proxy v1"
// ---------------------------------------------------------------------------

void ProxyControlPlaneService::HeadObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::HeadObjectRequest* /*request*/,
    ::us3_turbo::proxy::HeadObjectResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(PROXY_ERR_NOT_IMPLEMENTED, "%s",
                  std::string(ErrorMessage(PROXY_ERR_NOT_IMPLEMENTED)).c_str());
}

void ProxyControlPlaneService::GdsGet(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* /*request*/,
    ::us3_turbo::proxy::GdsChunkResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(PROXY_ERR_NOT_IMPLEMENTED, "%s",
                  std::string(ErrorMessage(PROXY_ERR_NOT_IMPLEMENTED)).c_str());
}

void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* /*request*/,
    ::us3_turbo::proxy::GdsChunkResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(PROXY_ERR_NOT_IMPLEMENTED, "%s",
                  std::string(ErrorMessage(PROXY_ERR_NOT_IMPLEMENTED)).c_str());
}

// 客户端失败路径会调 AbortSession；v1 happy path 不触发，留 SetFailed 占位。
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

void ProxyControlPlaneService::StartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::StartUploadRequest* /*request*/,
    ::us3_turbo::proxy::StartUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(PROXY_ERR_NOT_IMPLEMENTED, "%s",
                  std::string(ErrorMessage(PROXY_ERR_NOT_IMPLEMENTED)).c_str());
}

void ProxyControlPlaneService::AbortUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortUploadRequest* /*request*/,
    ::us3_turbo::proxy::AbortUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(PROXY_ERR_NOT_IMPLEMENTED, "%s",
                  std::string(ErrorMessage(PROXY_ERR_NOT_IMPLEMENTED)).c_str());
}

}  // namespace us3_turbo::proxy
