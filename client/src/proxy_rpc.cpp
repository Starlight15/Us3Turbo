#include "client/src/proxy_rpc.h"

#include <string>
#include <utility>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

namespace {

// 把 controller 失败填进 PutPathResult 并记日志，返回 false。
// is_timeout 分支保留，便于区分超时与数据面错误。
bool FailResult(PutPathResult& result, const brpc::Controller& cntl,
                std::string_view request_id, std::string_view op) {
  const bool is_timeout =
      (cntl.ErrorCode() == brpc::ERPCTIMEDOUT) ||
      (cntl.ErrorCode() == ETIMEDOUT);
  result.ok = false;
  result.error_message = cntl.ErrorText();
  spdlog::error("{} (req={}): failed to execute {} RPC: {}",
                is_timeout ? "timeout" : "data-plane",
                request_id, op, cntl.ErrorText());
  return false;
}

}  // namespace

bool ProxyRpc::GdsPut(std::string_view request_id,
                      const std::string& bucket,
                      const std::string& key,
                      std::uint64_t object_size,
                      const GdsDataSource& gds_source,
                      PutPathResult& result) const {
  if (!ok()) {
    spdlog::error("GdsPut (req={}): proxy channel not ready: {}",
                  request_id, init_error());
    result.ok = false;
    result.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller);

  us3_turbo::proxy::ClientProxyPutRequest rpc_request;
  rpc_request.set_request_id(std::string(request_id));
  rpc_request.set_bucket(bucket);
  rpc_request.set_key(key);
  rpc_request.set_object_size(object_size);
  rpc_request.set_path(us3_turbo::proxy::PATH_GDS);
  rpc_request.mutable_gds_source()->set_rdma_token(gds_source.rdma_token);

  us3_turbo::proxy::PutPathResult resp;
  stub()->GdsPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    return FailResult(result, controller, request_id, "GDS");
  }

  result.ok           = resp.ok();
  result.error_code   = resp.error_code();
  result.error_message = resp.error_message();
  result.etag         = resp.etag();
  result.crc32c       = resp.crc32c();
  result.bytes_written = resp.bytes_written();
  return resp.ok();
}

bool ProxyRpc::UcxPut(std::string_view request_id,
                      const std::string& bucket,
                      const std::string& key,
                      std::uint64_t object_size,
                      const UcxDataSource& ucx_source,
                      PutPathResult& result) const {
  if (!ok()) {
    spdlog::error("UcxPut (req={}): proxy channel not ready: {}",
                  request_id, init_error());
    result.ok = false;
    result.error_message = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller);

  us3_turbo::proxy::ClientProxyPutRequest rpc_request;
  rpc_request.set_request_id(std::string(request_id));
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
    return FailResult(result, controller, request_id, "UCX");
  }

  result.ok           = resp.ok();
  result.error_code   = resp.error_code();
  result.error_message = resp.error_message();
  result.etag         = resp.etag();
  result.crc32c       = resp.crc32c();
  result.bytes_written = resp.bytes_written();
  return resp.ok();
}

}  // namespace us3_turbo::client
