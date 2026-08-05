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
#include "us3_turbo/common/logger.h"

namespace us3_turbo::proxy {

// multipart part/complete/abort 无 bucket/key，Access 日志用占位符
static constexpr const char* kDash = "-";

/* 单步 PUT(GDS/RDMA): 薄委托, ret!=0 → SetFailed; 日志+Access 日志 */

void ProxyService::GdsPut(google::protobuf::RpcController* cntl_base,
                          const ClientProxyPutRequest* request, PutPathResult* response,
                          google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={} key={} size={}", request->bucket(), request->key(),
           request->object_size());

  PutOutput out;
  int ret = single_put_->PutGds(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    LOG_WARN(rid, "failed code={}", ret);
    cntl->SetFailed(ret, "%s", ProxyErrorMessage(ret));
    AccessLogger::Instance().LogRequest("GdsPut", rid, request->bucket(), request->key(), ret, 0,
                                        latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success etag={} bytes={}", out.etag, out.bytes);
  AccessLogger::Instance().LogRequest("GdsPut", rid, request->bucket(), request->key(), 0,
                                      out.bytes, latency);
}


void ProxyService::RdmaPut(google::protobuf::RpcController* cntl_base,
                           const ClientProxyPutRequest* request, PutPathResult* response,
                           google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={} key={} size={}", request->bucket(), request->key(),
           request->object_size());

  PutOutput out;
  int ret = single_put_->PutRdma(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    LOG_WARN(rid, "failed code={}", ret);
    cntl->SetFailed(ret, "%s", ProxyErrorMessage(ret));
    AccessLogger::Instance().LogRequest("RdmaPut", rid, request->bucket(), request->key(), ret, 0,
                                        latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success etag={} bytes={}", out.etag, out.bytes);
  AccessLogger::Instance().LogRequest("RdmaPut", rid, request->bucket(), request->key(), 0,
                                      out.bytes, latency);
}

/* 分段上传: 薄委托, ret!=0 → set_error_message+SetFailed; part/complete/abort
 * 无 bucket/key, 日志用"-"占位 */

void ProxyService::CreateMultipartUpload(google::protobuf::RpcController* cntl_base,
                                         const CreateMultipartUploadRequest* request,
                                         CreateMultipartUploadResponse* response,
                                         google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const std::string trace_id = utils::GenUuid();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={} key={} path={} trace_id={}", request->bucket(), request->key(),
           static_cast<int>(request->path()), trace_id);

  std::string upload_id;
  int ret =
      multipart_->CreateUpload(rid, request->bucket(), request->key(), request->path(), upload_id);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, trace_id);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest("CreateMultipartUpload", rid, request->bucket(),
                                        request->key(), ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_upload_id(upload_id);
  response->set_trace_id(trace_id);
  LOG_INFO(rid, "success upload_id={} trace_id={}", upload_id, trace_id);
  AccessLogger::Instance().LogRequest("CreateMultipartUpload", rid, request->bucket(),
                                      request->key(), 0, 0, latency);
}

void ProxyService::UploadPartGds(google::protobuf::RpcController* cntl_base,
                                 const UploadPartGdsRequest* request, UploadPartResponse* response,
                                 google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} part={} size={} trace_id={}", request->upload_id(), request->part_number(),
           request->part_size(), request->trace_id());

  UploadPartOutput out;
  int ret =
      multipart_->UploadPartGds(request->request_id(), request->upload_id(), request->part_number(),
                                request->part_size(), request->rdma_token(), out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, request->trace_id());
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "UploadPartGds failed: %s", msg);
    AccessLogger::Instance().LogRequest("UploadPartGds", rid, kDash, kDash, ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success part={} etag={} bytes={} trace_id={}", request->part_number(), out.etag,
           out.bytes, request->trace_id());
  AccessLogger::Instance().LogRequest("UploadPartGds", rid, kDash, kDash, 0, out.bytes,
                                      latency);
}


void ProxyService::UploadPartRdma(google::protobuf::RpcController* cntl_base,
                                  const UploadPartRdmaRequest* request,
                                  UploadPartResponse* response, google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} part={} size={} trace_id={}", request->upload_id(), request->part_number(),
           request->part_size(), request->trace_id());

  UploadPartOutput out;
  int ret = multipart_->UploadPartRdma(request->request_id(), request->upload_id(),
                                       request->part_number(), request->part_size(),
                                       request->rdma_token(), out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, request->trace_id());
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "UploadPartRdma failed: %s", msg);
    AccessLogger::Instance().LogRequest("UploadPartRdma", rid, kDash, kDash, ret, 0, latency);
    return;
  }
  response->set_ok(true);
  response->set_etag(out.etag);
  response->set_bytes_written(out.bytes);
  if (out.crc32c != 0) response->set_crc32c(out.crc32c);
  LOG_INFO(rid, "success part={} etag={} bytes={} trace_id={}", request->part_number(), out.etag,
           out.bytes, request->trace_id());
  AccessLogger::Instance().LogRequest("UploadPartRdma", rid, kDash, kDash, 0, out.bytes,
                                      latency);
}

void ProxyService::CompleteMultipartUpload(google::protobuf::RpcController* cntl_base,
                                           const CompleteMultipartUploadRequest* request,
                                           CompleteMultipartUploadResponse* response,
                                           google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} parts={} trace_id={}", request->upload_id(), request->parts_size(),
           request->trace_id());

  std::vector<CompleteMultipartUploadRequest_PartInfo> client_parts;
  client_parts.reserve(request->parts_size());
  for (int i = 0; i < request->parts_size(); ++i) {
    client_parts.push_back(request->parts(i));
  }

  CompleteOutput out;
  int ret = multipart_->CompleteUpload(rid, request->upload_id(), client_parts, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, request->trace_id());
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "complete failed: %s", msg);
    AccessLogger::Instance().LogRequest("CompleteMultipartUpload", rid, kDash, kDash, ret, 0,
                                        latency);
    return;
  }
  response->set_ok(true);
  response->set_object_id(out.object_id);
  response->set_etag(out.etag);
  response->set_object_size(out.object_size);
  LOG_INFO(rid, "success object_id={} size={} etag={} trace_id={}", out.object_id, out.object_size, out.etag,
           request->trace_id());
  AccessLogger::Instance().LogRequest("CompleteMultipartUpload", rid, kDash, kDash, 0,
                                      out.object_size, latency);
}

void ProxyService::AbortMultipartUpload(google::protobuf::RpcController* cntl_base,
                                        const AbortMultipartUploadRequest* request,
                                        AbortMultipartUploadResponse* response,
                                        google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  (void)static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start upload={} trace_id={}", request->upload_id(), request->trace_id());

  (void)multipart_->AbortUpload(rid, request->upload_id());  // 幂等
  response->set_ok(true);
  const auto latency = utils::ElapsedMs(start);
  LOG_INFO(rid, "done upload={} trace_id={}", request->upload_id(), request->trace_id());
  AccessLogger::Instance().LogRequest("AbortMultipartUpload", rid, kDash, kDash, 0, 0, latency);
}

/* GET(StatObject/GdsGet): 薄委托, ret!=0 → SetFailed */

void ProxyService::StatObject(google::protobuf::RpcController* cntl_base,
                              const StatObjectRequest* request, StatObjectResponse* response,
                              google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const std::string trace_id = utils::GenUuid();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={}/{} trace_id={}", request->bucket(), request->key(), trace_id);

  StatObjectOutput out;
  int ret = get_object_->StatObject(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, trace_id);
    response->set_ok(false);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest("StatObject", rid, request->bucket(), request->key(), ret,
                                        0, latency);
    return;
  }
  response->set_ok(true);
  response->set_object_size(out.object_size);
  response->set_block_size(out.block_size);
  response->set_hash(out.hash);
  response->set_trace_id(trace_id);
  LOG_INFO(rid, "success size={} block_size={} trace_id={}", out.object_size, out.block_size, trace_id);
  AccessLogger::Instance().LogRequest("StatObject", rid, request->bucket(), request->key(), 0,
                                      out.object_size, latency);
}

void ProxyService::GdsGet(google::protobuf::RpcController* cntl_base,
                          const ClientProxyGetRequest* request, GetPathResult* response,
                          google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={}/{} size={} trace_id={}", request->bucket(), request->key(),
           request->object_size(), request->trace_id());

  GetOutput out;
  int ret = get_object_->GetGds(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, request->trace_id());
    response->set_ok(false);
    response->set_error_code(ret);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest("GdsGet", rid, request->bucket(), request->key(), ret, 0,
                                        latency);
    return;
  }
  response->set_ok(true);
  response->set_crc32c(out.crc32c);
  response->set_bytes_read(out.bytes_read);
  response->set_hash(out.hash);
  LOG_INFO(rid, "success bytes={} hash={} trace_id={}", out.bytes_read, out.hash, request->trace_id());
  AccessLogger::Instance().LogRequest("GdsGet", rid, request->bucket(), request->key(), 0,
                                      out.bytes_read, latency);
}

void ProxyService::RdmaGet(google::protobuf::RpcController* cntl_base,
                           const ClientProxyGetRequest* request, GetPathResult* response,
                           google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string& rid = request->request_id();
  const auto start = std::chrono::steady_clock::now();
  LOG_INFO(rid, "start bucket={}/{} size={} trace_id={}", request->bucket(), request->key(),
           request->object_size(), request->trace_id());

  GetOutput out;
  int ret = get_object_->GetRdma(*request, out);
  const auto latency = utils::ElapsedMs(start);

  if (ret != 0) {
    const char* msg = ProxyErrorMessage(ret);
    LOG_WARN(rid, "failed code={} trace_id={}", ret, request->trace_id());
    response->set_ok(false);
    response->set_error_code(ret);
    response->set_error_message(msg);
    cntl->SetFailed(ret, "%s", msg);
    AccessLogger::Instance().LogRequest("RdmaGet", rid, request->bucket(), request->key(), ret, 0,
                                        latency);
    return;
  }
  response->set_ok(true);
  response->set_crc32c(out.crc32c);
  response->set_bytes_read(out.bytes_read);
  response->set_hash(out.hash);
  LOG_INFO(rid, "success bytes={} hash={} trace_id={}", out.bytes_read, out.hash, request->trace_id());
  AccessLogger::Instance().LogRequest("RdmaGet", rid, request->bucket(), request->key(), 0,
                                      out.bytes_read, latency);
}


}  // namespace us3_turbo::proxy
