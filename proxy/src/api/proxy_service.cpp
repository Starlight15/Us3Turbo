#include "proxy/src/api/proxy_service.h"

#include <string>
#include <utility>
#include <vector>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/access_logger.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

// bucket/key 在 multipart part/complete/abort 请求里没有，Access 日志用占位。
static constexpr const char* kDash = "-";

ProxyService::ProxyService(
    std::unique_ptr<SinglePut> single_put,
    std::unique_ptr<Multipart> multipart,
    std::unique_ptr<GetObject> get_object,
    IUploadIndex* index_for_cleanup)
    : single_put_(std::move(single_put)),
      multipart_(std::move(multipart)),
      get_object_(std::move(get_object)),
      index_(index_for_cleanup) {
  // 后台 TTL 清理：周期扫描删除过期会话；析构经 condition_variable 唤醒 join。
  cleanup_thread_ = std::thread([this]() { CleanupThreadMain(); });
}

ProxyService::~ProxyService() {
  {
    std::lock_guard lock(cleanup_mu_);
    stop_cleanup_ = true;
  }
  cleanup_cv_.notify_all();
  if (cleanup_thread_.joinable()) cleanup_thread_.join();
}

void ProxyService::CleanupThreadMain() {
  const std::int64_t kTtlMs = FLAGS_upload_ttl_ms;
  const auto kScanInterval =
      std::chrono::milliseconds(FLAGS_upload_ttl_scan_interval_ms);
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
}

// ===========================================================================
// 单步 PUT（GDS / UCX）：薄委托服务层，ret != 0 → SetFailed。
// 每个 handler 开始记 LOG_INFO，结束打 Access 日志（成功/失败均打）。
// ===========================================================================

void ProxyService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ClientProxyPutRequest* request,
    PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={} key={} size={}",
           request->bucket(), request->key(), request->object_size());

  PutOutput out;
  int ret = single_put_->PutGds(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    LOG_WARN(rid, "failed code={}", ret);
    cntl->SetFailed(ret, "%s", ProxyErrorMessage(ret));
    AccessLogger::Instance().LogRequest(
        "GdsPut", rid, request->bucket(), request->key(), ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success etag={} bytes={}", out.etag, out.bytes_written);
  AccessLogger::Instance().LogRequest(
      "GdsPut", rid, request->bucket(), request->key(),
      0, out.bytes_written, latency);
}

void ProxyService::UcxPut(
    google::protobuf::RpcController* cntl_base,
    const ClientProxyPutRequest* request,
    PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={} key={} size={}",
           request->bucket(), request->key(), request->object_size());

  PutOutput out;
  int ret = single_put_->PutUcx(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    LOG_WARN(rid, "failed code={}", ret);
    cntl->SetFailed(ret, "%s", ProxyErrorMessage(ret));
    AccessLogger::Instance().LogRequest(
        "UcxPut", rid, request->bucket(), request->key(), ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success etag={} bytes={}", out.etag, out.bytes_written);
  AccessLogger::Instance().LogRequest(
      "UcxPut", rid, request->bucket(), request->key(),
      0, out.bytes_written, latency);
}

// ===========================================================================
// 分段上传：薄委托服务层，ret != 0 → set_error_message + SetFailed。
// multipart part/complete/abort 请求无 bucket/key，Access 日志用 "-" 占位。
// ===========================================================================

void ProxyService::CreateMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const CreateMultipartUploadRequest* request,
    CreateMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={} key={} path={}",
           request->bucket(), request->key(), static_cast<int>(request->path()));

  std::string upload_id;
  int ret = multipart_->CreateUpload(rid, request->bucket(), request->key(),
                                     request->path(), upload_id);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={}", ret);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest(
        "CreateMultipartUpload", rid, request->bucket(), request->key(),
        ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_upload_id(upload_id);
  LOG_INFO(rid, "success upload_id={}", upload_id);
  AccessLogger::Instance().LogRequest(
      "CreateMultipartUpload", rid, request->bucket(), request->key(),
      0, 0, latency);
}

void ProxyService::UploadPartGds(
    google::protobuf::RpcController* cntl_base,
    const UploadPartGdsRequest* request,
    UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} part={} size={}",
           request->upload_id(), request->part_number(), request->part_size());

  UploadPartOutput out;
  int ret = multipart_->UploadPartGds(request->request_id(), request->upload_id(),
                                      request->part_number(), request->part_size(),
                                      request->rdma_token(), out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={}", ret);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "UploadPartGds failed: %s", msg);
    AccessLogger::Instance().LogRequest(
        "UploadPartGds", rid, kDash, kDash, ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success part={} etag={} bytes={}",
           request->part_number(), out.etag, out.bytes_written);
  AccessLogger::Instance().LogRequest(
      "UploadPartGds", rid, kDash, kDash, 0, out.bytes_written, latency);
}

void ProxyService::UploadPartUcx(
    google::protobuf::RpcController* cntl_base,
    const UploadPartUcxRequest* request,
    UploadPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} part={} size={}",
           request->upload_id(), request->part_number(), request->part_size());

  UploadPartOutput out;
  int ret = multipart_->UploadPartUcx(request->request_id(), request->upload_id(),
                                      request->part_number(), request->part_size(),
                                      request->remote_addr(), request->packed_rkey(),
                                      request->client_ucx_addr(), out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={}", ret);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "UploadPartUcx failed: %s", msg);
    AccessLogger::Instance().LogRequest(
        "UploadPartUcx", rid, kDash, kDash, ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes_written);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success part={} etag={} bytes={}",
           request->part_number(), out.etag, out.bytes_written);
  AccessLogger::Instance().LogRequest(
      "UploadPartUcx", rid, kDash, kDash, 0, out.bytes_written, latency);
}

void ProxyService::CompleteMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const CompleteMultipartUploadRequest* request,
    CompleteMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} parts={}",
           request->upload_id(), request->parts_size());

  std::vector<CompleteMultipartUploadRequest_PartInfo>
      client_parts;
  client_parts.reserve(request->parts_size());
  for (int i = 0; i < request->parts_size(); ++i) {
    client_parts.push_back(request->parts(i));
  }

  CompleteOutput out;
  int ret = multipart_->CompleteUpload(rid, request->upload_id(), client_parts, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={}", ret);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "complete failed: %s", msg);
    AccessLogger::Instance().LogRequest(
        "CompleteMultipartUpload", rid, kDash, kDash, ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_object_id(out.object_id);
  response->set_etag(out.etag);
  response->set_object_size(out.object_size);
  LOG_INFO(rid, "success object_id={} size={} etag={}",
           out.object_id, out.object_size, out.etag);
  AccessLogger::Instance().LogRequest(
      "CompleteMultipartUpload", rid, kDash, kDash,
      0, out.object_size, latency);
}

void ProxyService::AbortMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const AbortMultipartUploadRequest* request,
    AbortMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  (void)static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={}", request->upload_id());

  (void)multipart_->AbortUpload(rid, request->upload_id());  // 幂等，恒 true
  response->set_ok(true);
  const auto latency = utils::ElapsedMs(start);
  LOG_INFO(rid, "done upload={}", request->upload_id());
  AccessLogger::Instance().LogRequest(
      "AbortMultipartUpload", rid, kDash, kDash, 0, 0, latency);
}

// ===========================================================================
// GET（StatObject / GdsGet）：薄委托服务层，ret != 0 → SetFailed。
// ===========================================================================

void ProxyService::StatObject(
    google::protobuf::RpcController* cntl_base,
    const StatObjectRequest* request,
    StatObjectResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={}/{}", request->bucket(), request->key());

  StatObjectOutput out;
  int ret = get_object_->StatObject(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={}", ret);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest(
        "StatObject", rid, request->bucket(), request->key(), ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_object_size(out.object_size);
  response->set_block_size(out.block_size);
  response->set_hash(out.hash);
  LOG_INFO(rid, "success size={} block_size={}", out.object_size, out.block_size);
  AccessLogger::Instance().LogRequest(
      "StatObject", rid, request->bucket(), request->key(),
      0, out.object_size, latency);
}

void ProxyService::GdsGet(
    google::protobuf::RpcController* cntl_base,
    const ClientProxyGetRequest* request,
    GetPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={}/{} size={}",
           request->bucket(), request->key(), request->object_size());

  GetOutput out;
  int ret = get_object_->GetGds(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={}", ret);
    response->set_ok(false);
    response->set_error_code(ret);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest(
        "GdsGet", rid, request->bucket(), request->key(), ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_crc32c(out.crc32c);
  response->set_bytes_read(out.bytes_read);
  response->set_hash(out.hash);
  LOG_INFO(rid, "success bytes={} hash={}", out.bytes_read, out.hash);
  AccessLogger::Instance().LogRequest(
      "GdsGet", rid, request->bucket(), request->key(),
      0, out.bytes_read, latency);
}

}  // namespace us3_turbo::proxy
