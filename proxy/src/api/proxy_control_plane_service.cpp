#include "proxy/src/api/proxy_control_plane_service.h"

#include <chrono>
#include <utility>
#include <vector>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

ProxyControlPlaneService::ProxyControlPlaneService(
    std::unique_ptr<SinglePutService> single_put_svc,
    std::unique_ptr<MultipartService> multipart_svc,
    IUploadIndex* index_for_cleanup)
    : single_put_svc_(std::move(single_put_svc)),
      multipart_svc_(std::move(multipart_svc)),
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

ProxyControlPlaneService::~ProxyControlPlaneService() {
  {
    std::lock_guard lock(cleanup_mu_);
    stop_cleanup_ = true;
  }
  cleanup_cv_.notify_all();
  if (cleanup_thread_.joinable()) cleanup_thread_.join();
}

// ===========================================================================
// 单步 PUT（GDS / UCX）：薄委托服务层，status → response。
// ===========================================================================

void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto r = single_put_svc_->PutGds(*request);
  if (!r.status.ok()) {
    cntl->SetFailed(r.status.code, "%s", r.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(r.bytes_written);
  if (r.crc32c != 0) response->set_crc32c(r.crc32c);
  spdlog::info("GdsPut: forwarded etag={} crc32c={:x} bytes={}",
               r.etag, r.crc32c, r.bytes_written);
}

void ProxyControlPlaneService::UcxPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto r = single_put_svc_->PutUcx(*request);
  if (!r.status.ok()) {
    cntl->SetFailed(r.status.code, "%s", r.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(r.bytes_written);
  if (r.crc32c != 0) response->set_crc32c(r.crc32c);
  spdlog::info("UcxPut: forwarded etag={} crc32c={:x} bytes={}",
               r.etag, r.crc32c, r.bytes_written);
}

// ===========================================================================
// 分段上传：薄委托服务层。
// ===========================================================================

void ProxyControlPlaneService::CreateMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::CreateMultipartUploadRequest* request,
    ::us3_turbo::proxy::CreateMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto r = multipart_svc_->CreateUpload(request->bucket(), request->key(),
                                        request->path());
  if (!r.status.ok()) {
    response->set_ok(false);
    response->set_error_message(r.status.message);
    cntl->SetFailed(r.status.code, "%s", r.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_upload_id(r.upload_id);
  spdlog::info("CreateMultipartUpload: upload_id={} bucket={} key={} path={}",
               r.upload_id, request->bucket(), request->key(),
               static_cast<int>(request->path()));
}

void ProxyControlPlaneService::UploadPartGds(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::UploadPartGdsRequest* request,
    ::us3_turbo::proxy::UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto r = multipart_svc_->UploadPartGds(request->request_id(),
                                         request->upload_id(),
                                         request->part_number(),
                                         request->part_size(),
                                         request->rdma_token());
  if (!r.status.ok()) {
    response->set_ok(false);
    response->set_error_message(r.status.message);
    cntl->SetFailed(r.status.code, "UploadPartGds failed: %s",
                    r.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(r.bytes_written);
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

  auto r = multipart_svc_->UploadPartUcx(request->request_id(),
                                         request->upload_id(),
                                         request->part_number(),
                                         request->part_size(),
                                         request->remote_addr(),
                                         request->packed_rkey(),
                                         request->client_ucx_addr());
  if (!r.status.ok()) {
    response->set_ok(false);
    response->set_error_message(r.status.message);
    cntl->SetFailed(r.status.code, "UploadPartUcx failed: %s",
                    r.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(r.bytes_written);
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

  auto r = multipart_svc_->CompleteUpload(request->upload_id(), client_parts);
  if (!r.status.ok()) {
    response->set_ok(false);
    response->set_error_message(r.status.message);
    cntl->SetFailed(r.status.code, "complete failed: %s",
                    r.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_object_id(r.object_id);
  response->set_etag(r.etag);
  response->set_object_size(r.object_size);
  spdlog::info("CompleteMultipartUpload: upload={} object_id={} size={} etag={}",
               request->upload_id(), r.object_id, r.object_size, r.etag);
}

void ProxyControlPlaneService::AbortMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortMultipartUploadRequest* request,
    ::us3_turbo::proxy::AbortMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  (void)static_cast<brpc::Controller*>(cntl_base);
  (void)multipart_svc_->AbortUpload(request->upload_id());  // 幂等
  response->set_ok(true);
  spdlog::info("AbortMultipartUpload: upload={}", request->upload_id());
}

}  // namespace us3_turbo::proxy
