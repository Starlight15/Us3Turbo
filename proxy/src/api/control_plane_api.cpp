#include "proxy/src/api/control_plane_api.h"

#include <chrono>
#include <utility>
#include <vector>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

ControlPlaneApi::ControlPlaneApi(
    std::unique_ptr<SinglePut> single_put,
    std::unique_ptr<Multipart> multipart,
    IUploadIndex* index_for_cleanup)
    : single_put_(std::move(single_put)),
      multipart_(std::move(multipart)),
      index_(index_for_cleanup) {
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
      index_->RemoveExpired(kTtlMs);
      lock.lock();
    }
  });
}

ControlPlaneApi::~ControlPlaneApi() {
  {
    std::lock_guard lock(cleanup_mu_);
    stop_cleanup_ = true;
  }
  cleanup_cv_.notify_all();
  if (cleanup_thread_.joinable()) cleanup_thread_.join();
}

// ===========================================================================
// 单步 PUT（GDS / UCX）：薄委托服务层，false → SetFailed。
// ===========================================================================

void ControlPlaneApi::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  PutOutput out;
  ProxyError err;
  if (!single_put_->PutGds(*request, out, err)) {
    cntl->SetFailed(err.code, "%s", err.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  spdlog::info("GdsPut: forwarded etag={} crc32c={:x} bytes={}",
               out.etag, out.crc32c, out.bytes_written);
}

void ControlPlaneApi::UcxPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  PutOutput out;
  ProxyError err;
  if (!single_put_->PutUcx(*request, out, err)) {
    cntl->SetFailed(err.code, "%s", err.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  spdlog::info("UcxPut: forwarded etag={} crc32c={:x} bytes={}",
               out.etag, out.crc32c, out.bytes_written);
}

// ===========================================================================
// 分段上传：薄委托服务层，false → set_error_message + SetFailed。
// ===========================================================================

void ControlPlaneApi::CreateMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::CreateMultipartUploadRequest* request,
    ::us3_turbo::proxy::CreateMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  std::string upload_id;
  ProxyError err;
  if (!multipart_->CreateUpload(request->bucket(), request->key(),
                                request->path(), upload_id, err)) {
    response->set_ok(false);
    response->set_error_message(err.message);
    cntl->SetFailed(err.code, "%s", err.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_upload_id(upload_id);
  spdlog::info("CreateMultipartUpload: upload_id={} bucket={} key={} path={}",
               upload_id, request->bucket(), request->key(),
               static_cast<int>(request->path()));
}

void ControlPlaneApi::UploadPartGds(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::UploadPartGdsRequest* request,
    ::us3_turbo::proxy::UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  UploadPartOutput out;
  ProxyError err;
  if (!multipart_->UploadPartGds(request->request_id(), request->upload_id(),
                                 request->part_number(), request->part_size(),
                                 request->rdma_token(), out, err)) {
    response->set_ok(false);
    response->set_error_message(err.message);
    cntl->SetFailed(err.code, "UploadPartGds failed: %s", err.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  spdlog::info("UploadPartGds: upload={} part={} size={} etag={} crc={:x}",
               request->upload_id(), request->part_number(),
               request->part_size(), out.etag, out.crc32c);
}

void ControlPlaneApi::UploadPartUcx(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::UploadPartUcxRequest* request,
    ::us3_turbo::proxy::UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  UploadPartOutput out;
  ProxyError err;
  if (!multipart_->UploadPartUcx(request->request_id(), request->upload_id(),
                                 request->part_number(), request->part_size(),
                                 request->remote_addr(), request->packed_rkey(),
                                 request->client_ucx_addr(), out, err)) {
    response->set_ok(false);
    response->set_error_message(err.message);
    cntl->SetFailed(err.code, "UploadPartUcx failed: %s", err.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  spdlog::info("UploadPartUcx: upload={} part={} size={} etag={} crc={:x}",
               request->upload_id(), request->part_number(),
               request->part_size(), out.etag, out.crc32c);
}

void ControlPlaneApi::CompleteMultipartUpload(
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

  CompleteOutput out;
  ProxyError err;
  if (!multipart_->CompleteUpload(request->upload_id(), client_parts, out, err)) {
    response->set_ok(false);
    response->set_error_message(err.message);
    cntl->SetFailed(err.code, "complete failed: %s", err.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_object_id(out.object_id);
  response->set_etag(out.etag);
  response->set_object_size(out.object_size);
  spdlog::info("CompleteMultipartUpload: upload={} object_id={} size={} etag={}",
               request->upload_id(), out.object_id, out.object_size, out.etag);
}

void ControlPlaneApi::AbortMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortMultipartUploadRequest* request,
    ::us3_turbo::proxy::AbortMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  (void)static_cast<brpc::Controller*>(cntl_base);
  (void)multipart_->AbortUpload(request->upload_id());  // 幂等，恒 true
  response->set_ok(true);
  spdlog::info("AbortMultipartUpload: upload={}", request->upload_id());
}

}  // namespace us3_turbo::proxy
