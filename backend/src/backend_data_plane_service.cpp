#include "backend/src/backend_data_plane_service.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::backend {

namespace {

constexpr std::string_view kNotImplemented =
    "backend v1: only single-object GdsPut";

// 通知 proxy 的超时（仅 warn 不影响 GdsPut 返回）。
constexpr int kNotifyTimeoutMs = 1000;

[[nodiscard]] std::string BuildEtag(std::uint32_t crc) {
  char buf[9] = {};
  std::snprintf(buf, sizeof(buf), "%08x", crc);
  return std::string(buf);
}

[[nodiscard]] std::string BuildObjectId(const std::string& bucket,
                                        const std::string& object_key) {
  return bucket + "/" + object_key;
}

}  // namespace

BackendDataPlaneService::BackendDataPlaneService(
    BackendGdsSink& sink,
    std::string gateway_id,
    const std::string& proxy_endpoint)
    : sink_(sink), gateway_id_(std::move(gateway_id)) {
  if (!proxy_endpoint.empty()) {
    auto channel = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions options;
    options.timeout_ms = kNotifyTimeoutMs;
    options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
    if (channel->Init(proxy_endpoint.c_str(), nullptr, &options) != 0) {
      spdlog::warn("backend: failed to init proxy channel to {}, "
                   "ReportGdsPut disabled", proxy_endpoint);
      return;
    }
    proxy_channel_ = std::move(channel);
    proxy_stub_ = std::make_unique<
        ::us3_turbo::proxy::Control_Stub>(
        proxy_channel_.get());
    spdlog::info("backend: ReportGdsPut will notify proxy at {}",
                 proxy_endpoint);
  }
}

void BackendDataPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* request,
    ::us3_turbo::proxy::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // v1 只支持单对象分支：upload_id 必须为空、part_number 为 0。
  if (!request->upload_id().empty()) {
    cntl->SetFailed(std::string(kNotImplemented));
    return;
  }

  // v1 不校验 session/ticket（proxy 是另一进程）；只要求 token 非空、长度合法。
  if (request->rdma_token().empty()) {
    cntl->SetFailed("backend v1: missing rdma_token for GdsPut");
    return;
  }
  // chunk_size 取自 request；校验在 sink 内部（≤ 1 GiB）。
  const auto chunk_size = request->chunk_size();
  if (!sink_.available()) {
    cntl->SetFailed("backend v1: cuObjServer not available");
    return;
  }

  const std::string object_id =
      BuildObjectId(request->bucket(), request->object_key());
  auto outcome =
      sink_.ReceiveAndDiscard(object_id, request->rdma_token(), chunk_size);
  if (!outcome.ok) {
    cntl->SetFailed("backend v1: " + outcome.error);
    return;
  }

  // 合成响应：etag + bytes_received + crc32c（供 client 端 end-to-end 校验）。
  const std::string etag = BuildEtag(outcome.crc32c);
  response->set_etag(etag);
  response->set_bytes_received(outcome.bytes_transferred);
  response->set_crc32c(outcome.crc32c);

  // 通知 proxy 数据传输完成（异步、仅 warn，不影响对 client 的返回）。
  if (proxy_stub_ != nullptr) {
    brpc::Controller notify_cntl;
    notify_cntl.set_timeout_ms(kNotifyTimeoutMs);
    ::us3_turbo::proxy::ReportGdsPutRequest notify_req;
    notify_req.set_session_id(request->session_id());
    notify_req.set_etag(etag);
    notify_req.set_crc32c(outcome.crc32c);
    notify_req.set_bytes_transferred(outcome.bytes_transferred);
    ::us3_turbo::proxy::ReportGdsPutResponse notify_resp;
    proxy_stub_->ReportGdsPut(&notify_cntl, &notify_req, &notify_resp, nullptr);
    if (notify_cntl.Failed()) {
      spdlog::warn("backend: failed to notify proxy of GdsPut completion: {}",
                   notify_cntl.ErrorText());
    } else if (!notify_resp.accepted()) {
      spdlog::warn("backend: proxy rejected ReportGdsPut for session_id={}",
                   request->session_id());
    }
  }

  spdlog::info("backend.gdsput object={} bytes={} crc32c={:x}", object_id,
               outcome.bytes_transferred, outcome.crc32c);
}

// ---------------------------------------------------------------------------
// v1 占位方法：一律返回 "backend v1: only single-object GdsPut"
// ---------------------------------------------------------------------------

void BackendDataPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::OpenSessionRequest* /*request*/,
    ::us3_turbo::proxy::OpenSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::HeadObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::HeadObjectRequest* /*request*/,
    ::us3_turbo::proxy::HeadObjectResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::GdsGet(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* /*request*/,
    ::us3_turbo::proxy::GdsChunkResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortSessionRequest* /*request*/,
    ::us3_turbo::proxy::AbortSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::StartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::StartUploadRequest* /*request*/,
    ::us3_turbo::proxy::StartUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::CompleteUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::CompleteUploadRequest* /*request*/,
    ::us3_turbo::proxy::CompleteUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::AbortUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortUploadRequest* /*request*/,
    ::us3_turbo::proxy::AbortUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

}  // namespace us3_turbo::backend
