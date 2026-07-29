#include "client/src/rpc/proxy_rpc.h"

#include <string>
#include <utility>
#include <vector>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::client {

namespace {

// 把 controller 失败填进 PutPathResult 并记日志,区分超时与数据面错误。
bool FailResult(PutPathResult& res, const brpc::Controller& cntl, std::string_view req_id,
                std::string_view op) {
  const bool is_timeout =
      (cntl.ErrorCode() == brpc::ERPCTIMEDOUT) || (cntl.ErrorCode() == ETIMEDOUT);
  res.ok = false;
  res.error_message = cntl.ErrorText();
  LOG_ERROR(req_id, "{} RPC failed: {} (error={})", op, cntl.ErrorText(),
            is_timeout ? "timeout" : "data-plane");
  return false;
}

}  // namespace

bool ProxyRpc::GdsPut(std::string_view req_id, const std::string& bucket, const std::string& key,
                      std::uint64_t object_size, const std::string& rdma_token,
                      PutPathResult& res) const {
  if (!ok()) {
    LOG_ERROR(req_id, "proxy channel not ready: {}", init_error());
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller);

  us3_turbo::proxy::ClientProxyPutRequest rpc_request;
  rpc_request.set_request_id(std::string(req_id));
  rpc_request.set_bucket(bucket);
  rpc_request.set_key(key);
  rpc_request.set_object_size(object_size);
  rpc_request.set_path(us3_turbo::proxy::PATH_GDS);
  rpc_request.mutable_gds_source()->set_rdma_token(rdma_token);

  us3_turbo::proxy::PutPathResult resp;
  stub()->GdsPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    return FailResult(res, controller, req_id, "GDS");
  }

  res.ok = resp.ok();
  res.error_code = resp.error_code();
  res.error_message = resp.error_message();
  res.etag = resp.etag();
  res.crc32c = resp.crc32c();
  res.bytes = resp.bytes_written();
  return resp.ok();
}

bool ProxyRpc::RdmaPut(std::string_view req_id, const std::string& bucket, const std::string& key,
                       std::uint64_t object_size, const std::string& rdma_token,
                       PutPathResult& res) const {
  if (!ok()) {
    LOG_ERROR(req_id, "proxy channel not ready: {}", init_error());
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller);

  us3_turbo::proxy::ClientProxyPutRequest rpc_request;
  rpc_request.set_request_id(std::string(req_id));
  rpc_request.set_bucket(bucket);
  rpc_request.set_key(key);
  rpc_request.set_object_size(object_size);
  rpc_request.set_path(us3_turbo::proxy::PATH_RDMA);
  rpc_request.mutable_rdma_source()->set_rdma_token(rdma_token);

  us3_turbo::proxy::PutPathResult resp;
  stub()->RdmaPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    return FailResult(res, controller, req_id, "RDMA");
  }

  res.ok = resp.ok();
  res.error_code = resp.error_code();
  res.error_message = resp.error_message();
  res.etag = resp.etag();
  res.crc32c = resp.crc32c();
  res.bytes = resp.bytes_written();
  return resp.ok();
}

// ---------------------------------------------------------------------------
// 分段上传（client → proxy）。与单步 GdsPut/RdmaPut 共用同一 brpc channel
// 与 Control_Stub。
// ---------------------------------------------------------------------------

bool ProxyRpc::CreateMultipartUpload(std::string_view req_id, const std::string& bucket,
                                     const std::string& key, ::us3_turbo::proxy::PutDataPath path,
                                     std::string& out_upload_id, std::string& out_error) const {
  if (!ok()) {
    out_error = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::CreateMultipartUploadRequest req;
  req.set_request_id(std::string(req_id));
  req.set_bucket(bucket);
  req.set_key(key);
  req.set_path(path);

  ::us3_turbo::proxy::CreateMultipartUploadResponse resp;
  stub()->CreateMultipartUpload(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    out_error = controller.ErrorText();
    LOG_ERROR(req_id, "rpc failed: {}", controller.ErrorText());
    return false;
  }
  if (!resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  out_upload_id = resp.upload_id();
  return true;
}

bool ProxyRpc::UploadPartGds(std::string_view req_id, const std::string& upload_id,
                             std::uint32_t part_number, std::uint64_t part_size,
                             const std::string& rdma_token, PutPathResult& res) const {
  if (!ok()) {
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::UploadPartGdsRequest req;
  req.set_request_id(std::string(req_id));
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_part_size(part_size);
  req.set_rdma_token(rdma_token);

  ::us3_turbo::proxy::UploadPartResponse resp;
  stub()->UploadPartGds(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    return FailResult(res, controller, req_id, "UploadPartGds");
  }
  res.ok = resp.ok();
  res.error_message = resp.error_message();
  res.etag = resp.etag();
  res.bytes = resp.bytes_written();
  if (resp.has_crc32c()) res.crc32c = resp.crc32c();
  return resp.ok();
}

bool ProxyRpc::UploadPartRdma(std::string_view req_id, const std::string& upload_id,
                              std::uint32_t part_number, std::uint64_t part_size,
                              const std::string& rdma_token, PutPathResult& res) const {
  if (!ok()) {
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::UploadPartRdmaRequest req;
  req.set_request_id(std::string(req_id));
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_part_size(part_size);
  req.set_rdma_token(rdma_token);

  ::us3_turbo::proxy::UploadPartResponse resp;
  stub()->UploadPartRdma(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    return FailResult(res, controller, req_id, "UploadPartRdma");
  }
  res.ok = resp.ok();
  res.error_message = resp.error_message();
  res.etag = resp.etag();
  res.bytes = resp.bytes_written();
  if (resp.has_crc32c()) res.crc32c = resp.crc32c();
  return resp.ok();
}

bool ProxyRpc::CompleteMultipartUpload(
    std::string_view req_id, const std::string& upload_id,
    const std::vector<std::pair<std::uint32_t, std::string>>& parts,
    CompletedMultipart& out) const {
  if (!ok()) {
    out.error = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::CompleteMultipartUploadRequest req;
  req.set_request_id(std::string(req_id));
  req.set_upload_id(upload_id);
  for (const auto& [no, etag] : parts) {
    auto* p = req.add_parts();
    p->set_part_number(no);
    p->set_etag(etag);
  }

  ::us3_turbo::proxy::CompleteMultipartUploadResponse resp;
  stub()->CompleteMultipartUpload(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    out.error = controller.ErrorText();
    LOG_ERROR(req_id, "rpc failed: {}", controller.ErrorText());
    return false;
  }
  out.ok = resp.ok();
  out.object_id = resp.object_id();
  out.etag = resp.etag();
  out.object_size = resp.object_size();
  out.error = resp.error_message();
  return resp.ok();
}

bool ProxyRpc::AbortMultipartUpload(std::string_view req_id, const std::string& upload_id,
                                    std::string& out_error) const {
  if (!ok()) {
    out_error = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::AbortMultipartUploadRequest req;
  req.set_request_id(std::string(req_id));
  req.set_upload_id(upload_id);

  ::us3_turbo::proxy::AbortMultipartUploadResponse resp;
  stub()->AbortMultipartUpload(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    out_error = controller.ErrorText();
    LOG_ERROR(req_id, "rpc failed: {}", controller.ErrorText());
    return false;
  }
  if (!resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// GET（StatObject / GdsGet）
// ---------------------------------------------------------------------------

bool ProxyRpc::StatObject(std::string_view req_id, const std::string& bucket,
                          const std::string& key, std::uint64_t& out_object_size,
                          std::string& out_error) const {
  if (!ok()) {
    out_error = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::StatObjectRequest req;
  req.set_request_id(std::string(req_id));
  req.set_bucket(bucket);
  req.set_key(key);

  ::us3_turbo::proxy::StatObjectResponse resp;
  stub()->StatObject(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    out_error = controller.ErrorText();
    LOG_ERROR(req_id, "rpc failed: {}", controller.ErrorText());
    return false;
  }
  if (!resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  out_object_size = resp.object_size();
  return true;
}

bool ProxyRpc::GdsGet(std::string_view req_id, const std::string& bucket, const std::string& key,
                      std::uint64_t object_size, const std::string& rdma_token,
                      GetPathResult& res) const {
  if (!ok()) {
    LOG_ERROR(req_id, "proxy channel not ready: {}", init_error());
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller);

  us3_turbo::proxy::ClientProxyGetRequest rpc_request;
  rpc_request.set_request_id(std::string(req_id));
  rpc_request.set_bucket(bucket);
  rpc_request.set_key(key);
  rpc_request.set_object_size(object_size);
  rpc_request.mutable_gds_source()->set_rdma_token(rdma_token);

  us3_turbo::proxy::GetPathResult resp;
  stub()->GdsGet(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) || (controller.ErrorCode() == ETIMEDOUT);
    res.ok = false;
    res.error_message = controller.ErrorText();
    LOG_ERROR(req_id, "GdsGet RPC failed: {} (error={})", controller.ErrorText(),
              is_timeout ? "timeout" : "data-plane");
    return false;
  }

  res.ok = resp.ok();
  res.error_code = resp.error_code();
  res.error_message = resp.error_message();
  res.crc32c = resp.crc32c();
  res.bytes_read = resp.bytes_read();
  res.hash = resp.hash();
  return resp.ok();
}

bool ProxyRpc::RdmaGet(std::string_view req_id, const std::string& bucket, const std::string& key,
                       std::uint64_t object_size, const std::string& rdma_token,
                       GetPathResult& res) const {
  if (!ok()) {
    LOG_ERROR(req_id, "proxy channel not ready: {}", init_error());
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller);

  us3_turbo::proxy::ClientProxyGetRequest rpc_request;
  rpc_request.set_request_id(std::string(req_id));
  rpc_request.set_bucket(bucket);
  rpc_request.set_key(key);
  rpc_request.set_object_size(object_size);
  rpc_request.mutable_rdma_source()->set_rdma_token(rdma_token);

  us3_turbo::proxy::GetPathResult resp;
  stub()->RdmaGet(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) || (controller.ErrorCode() == ETIMEDOUT);
    res.ok = false;
    res.error_message = controller.ErrorText();
    LOG_ERROR(req_id, "RdmaGet RPC failed: {} (error={})", controller.ErrorText(),
              is_timeout ? "timeout" : "data-plane");
    return false;
  }

  res.ok = resp.ok();
  res.error_code = resp.error_code();
  res.error_message = resp.error_message();
  res.crc32c = resp.crc32c();
  res.bytes_read = resp.bytes_read();
  res.hash = resp.hash();
  return resp.ok();
}

}  // namespace us3_turbo::client
