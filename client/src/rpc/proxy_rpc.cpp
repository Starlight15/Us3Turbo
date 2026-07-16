#include "client/src/rpc/proxy_rpc.h"

#include <string>
#include <utility>
#include <vector>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

namespace {

// 把 controller 失败填进 PutPathResult 并记日志,区分超时与数据面错误。
bool FailResult(PutPathResult& res, const brpc::Controller& cntl,
                std::string_view req_id, std::string_view op) {
  const bool is_timeout = (cntl.ErrorCode() == brpc::ERPCTIMEDOUT) ||
                          (cntl.ErrorCode() == ETIMEDOUT);
  res.ok = false;
  res.error_message = cntl.ErrorText();
  spdlog::error("{} (req={}): failed to execute {} RPC: {}",
                is_timeout ? "timeout" : "data-plane", req_id, op,
                cntl.ErrorText());
  return false;
}

}  // namespace

bool ProxyRpc::GdsPut(std::string_view req_id, const std::string& bucket,
                      const std::string& key, std::uint64_t object_size,
                      const GdsDataSource& gds_source,
                      PutPathResult& res) const {
  if (!ok()) {
    spdlog::error("GdsPut (req={}): proxy channel not ready: {}", req_id,
                  init_error());
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
  rpc_request.mutable_gds_source()->set_rdma_token(gds_source.rdma_token);

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
  res.bytes_written = resp.bytes_written();
  return resp.ok();
}

bool ProxyRpc::UcxPut(std::string_view req_id, const std::string& bucket,
                      const std::string& key, std::uint64_t object_size,
                      const UcxDataSource& ucx_source,
                      PutPathResult& res) const {
  if (!ok()) {
    spdlog::error("UcxPut (req={}): proxy channel not ready: {}", req_id,
                  init_error());
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
  rpc_request.set_path(us3_turbo::proxy::PATH_UCX);
  auto* ucx = rpc_request.mutable_ucx_source();
  ucx->set_remote_addr(ucx_source.remote_addr);
  ucx->set_packed_rkey(ucx_source.packed_rkey);
  ucx->set_client_ucx_addr(ucx_source.client_ucx_addr);

  us3_turbo::proxy::PutPathResult resp;
  stub()->UcxPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    return FailResult(res, controller, req_id, "UCX");
  }

  res.ok = resp.ok();
  res.error_code = resp.error_code();
  res.error_message = resp.error_message();
  res.etag = resp.etag();
  res.crc32c = resp.crc32c();
  res.bytes_written = resp.bytes_written();
  return resp.ok();
}

// ---------------------------------------------------------------------------
// 分段上传（client → proxy）。与单步 GdsPut/UcxPut 共用同一 brpc channel
// 与 Control_Stub；proxy 在 Control service 上同时暴露这 4 个 RPC。
// ---------------------------------------------------------------------------

bool ProxyRpc::CreateMultipartUpload(std::string_view req_id,
                                     const std::string& bucket,
                                     const std::string& key,
                                     ::us3_turbo::proxy::PutDataPath path,
                                     std::string& out_upload_id,
                                     std::string& out_error) const {
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
    spdlog::error("CreateMultipartUpload (req={}): rpc failed: {}", req_id,
                  controller.ErrorText());
    return false;
  }
  if (!resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  out_upload_id = resp.upload_id();
  return true;
}

bool ProxyRpc::UploadPartGds(std::string_view req_id,
                             const std::string& upload_id,
                             std::uint32_t part_number, std::uint64_t part_size,
                             const std::string& rdma_token,
                             PutPathResult& res) const {
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
  res.bytes_written = resp.bytes_written();
  if (resp.has_crc32c()) res.crc32c = resp.crc32c();
  return resp.ok();
}

bool ProxyRpc::UploadPartUcx(std::string_view req_id,
                             const std::string& upload_id,
                             std::uint32_t part_number, std::uint64_t part_size,
                             std::uint64_t remote_addr,
                             const std::string& packed_rkey,
                             const std::string& client_ucx_addr,
                             PutPathResult& res) const {
  if (!ok()) {
    res.ok = false;
    res.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::UploadPartUcxRequest req;
  req.set_request_id(std::string(req_id));
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_part_size(part_size);
  req.set_remote_addr(remote_addr);
  req.set_packed_rkey(packed_rkey);
  req.set_client_ucx_addr(client_ucx_addr);

  ::us3_turbo::proxy::UploadPartResponse resp;
  stub()->UploadPartUcx(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    return FailResult(res, controller, req_id, "UploadPartUcx");
  }
  res.ok = resp.ok();
  res.error_message = resp.error_message();
  res.etag = resp.etag();
  res.bytes_written = resp.bytes_written();
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
    spdlog::error("CompleteMultipartUpload (req={}): rpc failed: {}", req_id,
                  controller.ErrorText());
    return false;
  }
  out.ok = resp.ok();
  out.object_id = resp.object_id();
  out.etag = resp.etag();
  out.object_size = resp.object_size();
  out.error = resp.error_message();
  return resp.ok();
}

bool ProxyRpc::AbortMultipartUpload(std::string_view req_id,
                                    const std::string& upload_id,
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
    spdlog::error("AbortMultipartUpload (req={}): rpc failed: {}", req_id,
                  controller.ErrorText());
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
                          const std::string& key,
                          std::uint64_t& out_object_size,
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
    spdlog::error("StatObject (req={}): rpc failed: {}", req_id,
                  controller.ErrorText());
    return false;
  }
  if (!resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  out_object_size = resp.object_size();
  return true;
}

bool ProxyRpc::GdsGet(std::string_view req_id, const std::string& bucket,
                      const std::string& key, std::uint64_t object_size,
                      const GdsDataSource& gds_source,
                      GetPathResult& res) const {
  if (!ok()) {
    spdlog::error("GdsGet (req={}): proxy channel not ready: {}", req_id,
                  init_error());
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
  rpc_request.mutable_gds_source()->set_rdma_token(gds_source.rdma_token);

  us3_turbo::proxy::GetPathResult resp;
  stub()->GdsGet(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout = (controller.ErrorCode() == brpc::ERPCTIMEDOUT) ||
                            (controller.ErrorCode() == ETIMEDOUT);
    res.ok = false;
    res.error_message = controller.ErrorText();
    spdlog::error("{} (req={}): failed to execute GdsGet RPC: {}",
                  is_timeout ? "timeout" : "data-plane", req_id,
                  controller.ErrorText());
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

bool ProxyRpc::UcxGet(std::string_view req_id, const std::string& bucket,
                      const std::string& key, std::uint64_t object_size,
                      const UcxDataSource& ucx_source,
                      GetPathResult& res) const {
  if (!ok()) {
    spdlog::error("UcxGet (req={}): proxy channel not ready: {}", req_id,
                  init_error());
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
  auto* ucx = rpc_request.mutable_ucx_source();
  ucx->set_remote_addr(ucx_source.remote_addr);
  ucx->set_packed_rkey(ucx_source.packed_rkey);
  ucx->set_client_ucx_addr(ucx_source.client_ucx_addr);

  us3_turbo::proxy::GetPathResult resp;
  stub()->UcxGet(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout = (controller.ErrorCode() == brpc::ERPCTIMEDOUT) ||
                            (controller.ErrorCode() == ETIMEDOUT);
    res.ok = false;
    res.error_message = controller.ErrorText();
    spdlog::error("{} (req={}): failed to execute UcxGet RPC: {}",
                  is_timeout ? "timeout" : "data-plane", req_id,
                  controller.ErrorText());
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
