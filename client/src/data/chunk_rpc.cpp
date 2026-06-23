#include "client/src/data/chunk_rpc.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/common/brpc_util.h"
#include "client/src/common/errors.h"

namespace us3_turbo::client {

ChunkRpc::ChunkRpc(const std::string& endpoint, std::chrono::milliseconds timeout) {
  // channel 由 InitBrpcChannel 统一构建:endpoint 为空或 Init 失败时返回
  // nullptr、init_error_ 存原因,ok() 返回 false。
  channel_ = InitBrpcChannel(endpoint, timeout, init_error_, "data");
  if (!channel_) {
    return;
  }
  stub_ = std::make_unique<us3_turbo::proxy::Control_Stub>(channel_.get());
}

Result<us3_turbo::proxy::GdsChunkResponse>
ChunkRpc::Put(const GdsChunkRequest& request) const {
  if (!ok()) {
    return Result<us3_turbo::proxy::GdsChunkResponse>::Failure(
        MakeError(ErrorCode::kRpcError, init_error_, /*retryable=*/true));
  }
  brpc::Controller controller;
  ApplyRequestHeaders(controller, request.context);

  us3_turbo::proxy::GdsChunkRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_transfer_ticket(request.transfer_ticket);
  rpc_request.set_bucket(request.bucket);
  rpc_request.set_object_key(request.key);
  rpc_request.set_data_flow(std::string(ToString(DataFlow::GPUDirect)));
  rpc_request.set_chunk_offset(request.chunk_offset);
  rpc_request.set_chunk_size(request.chunk_size);
  rpc_request.set_rdma_token(request.rdma_token);

  us3_turbo::proxy::GdsChunkResponse rpc_response;
  stub_->GdsPut(&controller, &rpc_request, &rpc_response, nullptr);

  // 数据面 RPC:非超时失败归为 kTransportError(而非控制面错误)。
  auto status = CheckRpcFailure(controller, "Failed to execute GDS chunk RPC",
                                DataFlow::GPUDirect, request.request_id,
                                /*is_data_plane=*/true);
  if (!status.success()) {
    return Result<us3_turbo::proxy::GdsChunkResponse>::Failure(status.error());
  }
  return Result<us3_turbo::proxy::GdsChunkResponse>::Success(std::move(rpc_response));
}

}  // namespace us3_turbo::client
