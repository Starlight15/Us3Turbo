#include "proxy/src/service/proxy_control_plane_service.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/utils.h"

namespace us3_turbo::proxy {

namespace {

// 单步 PUT 对象上限：16 MiB。超出应走分段上传。
// 各模块内各自定义同名常量（client/proxy/backend），不跨模块共享头。
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024;

}  // namespace

ProxyControlPlaneService::ProxyControlPlaneService(
    std::string backend_endpoint,
    int backend_timeout_ms)
    : backend_timeout_ms_(backend_timeout_ms) {
  if (backend_endpoint.empty()) {
    spdlog::warn("proxy: backend_endpoint empty, GdsPut will reject as "
                 "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }
  // 单步 GdsPut/UcxPut 用的 SINGLE channel。
  auto channel = std::make_shared<brpc::Channel>();
  brpc::ChannelOptions options;
  options.timeout_ms = backend_timeout_ms_;
  options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
  if (channel->Init(backend_endpoint.c_str(), nullptr, &options) != 0) {
    spdlog::warn("proxy: failed to init backend channel to {}, GdsPut disabled",
                 backend_endpoint);
    return;
  }
  backend_channel_ = std::move(channel);
  backend_stub_ = std::make_unique<::us3_turbo::proxy::Control_Stub>(
      backend_channel_.get());

  // block 级 POOLED channel（分段上传用）：独立连接池，避免与单步 SINGLE 串行阻塞。
  auto block_channel = std::make_shared<brpc::Channel>();
  brpc::ChannelOptions block_opts;
  block_opts.timeout_ms = backend_timeout_ms_;
  block_opts.connection_type = brpc::CONNECTION_TYPE_POOLED;
  if (block_channel->Init(backend_endpoint.c_str(), nullptr, &block_opts) != 0) {
    spdlog::warn("proxy: failed to init backend block channel; multipart disabled");
  } else {
    backend_block_channel_ = std::move(block_channel);
    backend_block_stub_ =
        std::make_unique<::us3_turbo::proxy::BackendDataPlane_Stub>(
            backend_block_channel_.get());
    put_handler_ = std::make_unique<MultipartPutHandler>(
        backend_block_stub_.get(), backend_timeout_ms_);
  }
  spdlog::info("proxy: backend forward channel ready at {} (timeout {}ms)",
               backend_endpoint, backend_timeout_ms_);

  // 后台 TTL 清理：每小时扫一次，删 3 天前的会话；析构经 condition_variable 唤醒 join。
  cleanup_thread_ = std::thread([this]() {
    constexpr std::int64_t kTtlMs = 3LL * 24 * 3600 * 1000;
    constexpr auto kScanInterval = std::chrono::hours(1);
    std::unique_lock lock(cleanup_mu_);
    while (!stop_cleanup_) {
      if (cleanup_cv_.wait_for(lock, kScanInterval,
                               [this] { return stop_cleanup_; })) {
        break;  // 被析构唤醒
      }
      lock.unlock();
      session_manager_.CleanupExpiredSessions(kTtlMs);
      lock.lock();
    }
  });
}

ProxyControlPlaneService::~ProxyControlPlaneService() {
  {
    std::lock_guard lock(cleanup_mu_);
    stop_cleanup_ = true;
  }
  cleanup_cv_.notify_all();
  if (cleanup_thread_.joinable()) cleanup_thread_.join();
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
  if (request->object_size() > kMaxUploadBytes) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "object_size exceeds 16MiB single-step limit; use multipart");
    return;
  }
  if (request->path() != ::us3_turbo::proxy::PATH_GDS) {
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED, "GdsPut requires PATH_GDS");
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

  response->CopyFrom(bresp);

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
  if (request->object_size() > kMaxUploadBytes) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                    "object_size exceeds 16MiB single-step limit; use multipart");
    return;
  }
  if (request->path() != ::us3_turbo::proxy::PATH_UCX) {
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED, "UcxPut requires PATH_UCX");
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

  response->CopyFrom(bresp);

  spdlog::info("UcxPut: forwarded etag={} crc32c={:x} bytes={}",
               bresp.etag(), bresp.crc32c(), bresp.bytes_written());
}

// ===========================================================================
// 分段上传（client → proxy）。单步 GdsPut/UcxPut 与之完全隔离。
// ===========================================================================

void ProxyControlPlaneService::CreateMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::CreateMultipartUploadRequest* request,
    ::us3_turbo::proxy::CreateMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  if (request->bucket().empty() || request->key().empty()) {
    response->set_ok(false);
    response->set_error_message("bucket or key is empty");
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "bucket or key is empty");
    return;
  }
  if (request->path() != ::us3_turbo::proxy::PATH_GDS &&
      request->path() != ::us3_turbo::proxy::PATH_UCX) {
    response->set_ok(false);
    response->set_error_message("path must be PATH_GDS or PATH_UCX");
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED,
                    "path must be PATH_GDS or PATH_UCX");
    return;
  }

  const std::string upload_id = session_manager_.CreateSession(
      request->bucket(), request->key(), request->path());
  response->set_ok(true);
  response->set_upload_id(upload_id);

  spdlog::info("CreateMultipartUpload: upload_id={} bucket={} key={} path={}",
               upload_id, request->bucket(), request->key(),
               static_cast<int>(request->path()));
}

void ProxyControlPlaneService::UploadPartGds(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::UploadPartGdsRequest* request,
    ::us3_turbo::proxy::UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  ::us3_turbo::proxy::PutDataPath path{};
  if (!session_manager_.GetSessionPath(request->upload_id(), path)) {
    response->set_ok(false);
    response->set_error_message("upload_id not found");
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "upload_id not found");
    return;
  }
  if (path != ::us3_turbo::proxy::PATH_GDS) {
    response->set_ok(false);
    response->set_error_message("session path is not PATH_GDS");
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED,
                    "session path is not PATH_GDS");
    return;
  }
  if (request->part_number() == 0 || request->part_size() == 0) {
    response->set_ok(false);
    response->set_error_message("part_number or part_size is zero");
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "part_number or part_size is zero");
    return;
  }
  if (request->rdma_token().empty()) {
    response->set_ok(false);
    response->set_error_message("rdma_token is empty");
    cntl->SetFailed(PROXY_ERR_MISSING_SOURCE, "rdma_token is empty");
    return;
  }

  // 切分 + 并发上传到 backend。
  auto r = put_handler_->HandleGdsPart(request->request_id(),
                                       request->upload_id(),
                                       request->part_number(),
                                       request->part_size(),
                                       request->rdma_token());
  if (!r.ok) {
    response->set_ok(false);
    response->set_error_message(r.error);
    cntl->SetFailed(PROXY_ERR_BACKEND_RPC, "UploadPartGds failed: %s",
                    r.error.c_str());
    return;
  }

  PartMetadata part;
  part.part_number = request->part_number();
  part.part_size = request->part_size();
  part.etag = r.etag;
  part.upload_time_ms = utils::NowMs();
  session_manager_.AddPart(request->upload_id(), part);

  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(request->part_size());
  if (r.crc32c != 0) response->set_crc32c(r.crc32c);

  spdlog::info("UploadPartGds: upload={} part={} size={} etag={} crc={:x}",
               request->upload_id(), request->part_number(),
               request->part_size(), r.etag, r.crc32c);
}

void ProxyControlPlaneService::UploadPartUcx(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::UploadPartUcxRequest* request,
    ::us3_turbo::proxy::UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  ::us3_turbo::proxy::PutDataPath path{};
  if (!session_manager_.GetSessionPath(request->upload_id(), path)) {
    response->set_ok(false);
    response->set_error_message("upload_id not found");
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "upload_id not found");
    return;
  }
  if (path != ::us3_turbo::proxy::PATH_UCX) {
    response->set_ok(false);
    response->set_error_message("session path is not PATH_UCX");
    cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED,
                    "session path is not PATH_UCX");
    return;
  }
  if (request->part_number() == 0 || request->part_size() == 0) {
    response->set_ok(false);
    response->set_error_message("part_number or part_size is zero");
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "part_number or part_size is zero");
    return;
  }
  if (request->remote_addr() == 0 || request->packed_rkey().empty() ||
      request->client_ucx_addr().empty()) {
    response->set_ok(false);
    response->set_error_message("ucx source fields incomplete");
    cntl->SetFailed(PROXY_ERR_MISSING_SOURCE, "ucx source fields incomplete");
    return;
  }

  auto r = put_handler_->HandleUcxPart(request->request_id(),
                                       request->upload_id(),
                                       request->part_number(),
                                       request->part_size(),
                                       request->remote_addr(),
                                       request->packed_rkey(),
                                       request->client_ucx_addr());
  if (!r.ok) {
    response->set_ok(false);
    response->set_error_message(r.error);
    cntl->SetFailed(PROXY_ERR_BACKEND_RPC, "UploadPartUcx failed: %s",
                    r.error.c_str());
    return;
  }

  PartMetadata part;
  part.part_number = request->part_number();
  part.part_size = request->part_size();
  part.etag = r.etag;
  part.upload_time_ms = utils::NowMs();
  session_manager_.AddPart(request->upload_id(), part);

  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(request->part_size());
  if (r.crc32c != 0) response->set_crc32c(r.crc32c);

  spdlog::info("UploadPartUcx: upload={} part={} size={} etag={} crc={:x}",
               request->upload_id(), request->part_number(),
               request->part_size(), r.etag, r.crc32c);
}

void ProxyControlPlaneService::CompleteMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::CompleteMultipartUploadRequest* request,
    ::us3_turbo::proxy::CompleteMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>
      client_parts;
  client_parts.reserve(request->parts_size());
  for (int i = 0; i < request->parts_size(); ++i) {
    client_parts.push_back(request->parts(i));
  }

  std::string object_id, etag, error;
  std::uint64_t object_size = 0;
  const bool ok = session_manager_.CompleteSession(
      request->upload_id(), client_parts, object_id, etag, object_size, error);
  if (!ok) {
    response->set_ok(false);
    response->set_error_message(error);
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "complete failed: %s",
                    error.c_str());
    return;
  }

  // 成功后立即清理会话。
  session_manager_.CleanupSession(request->upload_id());

  response->set_ok(true);
  response->set_object_id(object_id);
  response->set_etag(etag);
  response->set_object_size(object_size);

  spdlog::info("CompleteMultipartUpload: upload={} object_id={} size={} etag={}",
               request->upload_id(), object_id, object_size, etag);
}

void ProxyControlPlaneService::AbortMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortMultipartUploadRequest* request,
    ::us3_turbo::proxy::AbortMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  (void)cntl;
  session_manager_.CleanupSession(request->upload_id());  // 幂等，不存在也 ok
  response->set_ok(true);
  spdlog::info("AbortMultipartUpload: upload={}", request->upload_id());
}

}  // namespace us3_turbo::proxy
