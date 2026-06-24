#include "client/src/data/chunk_rpc.h"

#include <utility>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

bool ChunkRpc::Put(const GdsChunkRequest& request,
                  GdsPutResult& out) const {
  if (!ok()) {
    spdlog::error("GdsPut (req={}): data-plane channel not ready: {}",
                  request.request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, request.timeout);

  us3_turbo::proxy::GdsChunkRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_transfer_ticket(request.transfer_ticket);
  rpc_request.set_bucket(request.bucket);
  rpc_request.set_object_key(request.key);
  rpc_request.set_data_flow(std::string(ToString(DataFlow::GPUDirect)));
  rpc_request.set_chunk_size(request.chunk_size);
  rpc_request.set_rdma_token(request.rdma_token);

  us3_turbo::proxy::GdsChunkResponse resp;
  stub()->GdsPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) || (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to execute GDS chunk RPC: {}",
                  is_timeout ? "timeout" : "data-plane",
                  request.request_id, controller.ErrorText());
    return false;
  }

  out.etag = resp.etag();
  return true;
}

}  // namespace us3_turbo::client
