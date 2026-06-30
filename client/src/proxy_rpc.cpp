#include "client/src/proxy_rpc.h"

#include <string>
#include <utility>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

bool ProxyRpc::Put(const PutObjectRequest& request,
                   std::string_view request_id,
                   std::chrono::milliseconds timeout,
                   std::string_view rdma_token,
                   std::size_t chunk_size,
                   GdsPutResult& result) const {
  if (!ok()) {
    spdlog::error("GdsPut (req={}): proxy channel not ready: {}",
                  request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, timeout);

  us3_turbo::proxy::GdsChunkRequest rpc_request;
  rpc_request.set_request_id(std::string(request_id));
  rpc_request.set_bucket(request.bucket);
  rpc_request.set_object_key(request.key);
  rpc_request.set_chunk_size(chunk_size);
  rpc_request.set_rdma_token(std::string(rdma_token));

  us3_turbo::proxy::GdsChunkResponse resp;
  stub()->GdsPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) ||
        (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to execute GDS chunk RPC: {}",
                  is_timeout ? "timeout" : "data-plane",
                  request_id, controller.ErrorText());
    return false;
  }

  result.etag = resp.etag();
  result.crc32c = resp.crc32c();
  return true;
}

bool ProxyRpc::RdmaPut(const PutObjectRequest& request,
                       std::string_view request_id,
                       std::chrono::milliseconds timeout,
                       std::uint64_t remote_addr,
                       std::string_view rkey,
                       std::string_view client_ucx_addr,
                       std::size_t chunk_size,
                       GdsPutResult& result) const {
  if (!ok()) {
    spdlog::error("RdmaPut (req={}): proxy channel not ready: {}",
                  request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, timeout);

  us3_turbo::proxy::RdmaChunkRequest rpc_request;
  rpc_request.set_request_id(std::string(request_id));
  rpc_request.set_bucket(request.bucket);
  rpc_request.set_object_key(request.key);
  rpc_request.set_chunk_size(chunk_size);
  rpc_request.set_remote_addr(remote_addr);
  rpc_request.set_rkey(std::string(rkey));
  rpc_request.set_client_ucx_addr(std::string(client_ucx_addr));

  us3_turbo::proxy::RdmaChunkResponse resp;
  stub()->RdmaPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) ||
        (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to execute RDMA chunk RPC: {}",
                  is_timeout ? "timeout" : "data-plane",
                  request_id, controller.ErrorText());
    return false;
  }

  result.etag = resp.etag();
  result.crc32c = resp.crc32c();
  return true;
}

}  // namespace us3_turbo::client
