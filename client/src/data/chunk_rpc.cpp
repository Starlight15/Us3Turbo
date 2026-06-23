#include "client/src/data/chunk_rpc.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/common/errors.h"

namespace us3_turbo::client {

Result<us3_turbo::proxy::GdsChunkResponse>
ChunkRpc::Put(const GdsChunkRequest& request) const {
  if (!ok()) {
    return Result<us3_turbo::proxy::GdsChunkResponse>::Failure(
        Fail(init_error_code(), init_error(), /*retryable=*/true,
             std::string(ToString(DataFlow::GPUDirect))));
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

  us3_turbo::proxy::GdsChunkResponse rpc_response;
  stub()->GdsPut(&controller, &rpc_request, &rpc_response, nullptr);

  auto status = CheckFailure(controller, "Failed to execute GDS chunk RPC",
                             request.request_id);
  if (!status.success()) {
    return Result<us3_turbo::proxy::GdsChunkResponse>::Failure(status.error());
  }
  return Result<us3_turbo::proxy::GdsChunkResponse>::Success(std::move(rpc_response));
}

}  // namespace us3_turbo::client
