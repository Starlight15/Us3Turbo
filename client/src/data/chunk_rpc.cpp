#include "client/src/data/chunk_rpc.h"

#include <utility>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

bool ChunkRpc::Put(const PutAttempt& attempt,
                  const SessionGrant& grant,
                  std::string_view rdma_token,
                  std::size_t chunk_size,
                  GdsPutResult& result) const {
  if (!ok()) {
    spdlog::error("GdsPut (req={}): data-plane channel not ready: {}",
                  attempt.request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, attempt.timeout);

  us3_turbo::proxy::GdsChunkRequest rpc_request;
  rpc_request.set_request_id(attempt.request_id);
  rpc_request.set_session_id(attempt.session_id);
  rpc_request.set_transfer_ticket(grant.ticket);
  rpc_request.set_bucket(attempt.bucket);
  rpc_request.set_object_key(attempt.key);
  rpc_request.set_data_flow(std::string(ToString(DataFlow::GPUDirect)));
  rpc_request.set_chunk_size(chunk_size);
  rpc_request.set_rdma_token(std::string(rdma_token));

  us3_turbo::proxy::GdsChunkResponse resp;
  stub()->GdsPut(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) || (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to execute GDS chunk RPC: {}",
                  is_timeout ? "timeout" : "data-plane",
                  attempt.request_id, controller.ErrorText());
    return false;
  }

  result.etag = resp.etag();
  return true;
}

}  // namespace us3_turbo::client
