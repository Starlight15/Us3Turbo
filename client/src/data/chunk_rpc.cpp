#include "client/src/data/chunk_rpc.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/common/brpc_util.h"
#include "client/src/common/errors.h"

namespace us3_turbo::client {
namespace {

// 去尾斜杠,让 "host:port/" 与 "host:port" 等价。
[[nodiscard]] std::string TrimTrailingSlash(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint;
}

}  // namespace

ChunkRpc::ChunkRpc(const std::string& endpoint, std::chrono::milliseconds timeout) {
  // 参数校验:endpoint 为空直接失败,不建 channel。channel_ / stub_ 留空,
  // ok() 返回 false,init_error_ 存原因。
  if (endpoint.empty()) {
    init_error_ = "data endpoint must not be empty";
    return;
  }
  // 就地 Init 一条 baidu_std channel:connect / RPC 超时同源,max_retry=2。
  // Init 失败时 channel_ 留空、init_error_ 存原因,ok() 返回 false。
  channel_ = std::make_unique<brpc::Channel>();
  brpc::ChannelOptions co;
  co.protocol           = "baidu_std";
  co.connect_timeout_ms = static_cast<int>(timeout.count());
  co.timeout_ms         = static_cast<int>(timeout.count());
  co.max_retry          = 2;
  if (channel_->Init(TrimTrailingSlash(endpoint).c_str(), nullptr, &co) != 0) {
    init_error_ = "Failed to initialize brpc channel: " + endpoint;
    channel_.reset();
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
  rpc_request.set_data_flow(std::string(ToString(request.data_flow)));
  rpc_request.set_buffer_type(std::string(ToString(request.buffer_type)));
  rpc_request.set_checksum_policy(request.checksum_policy);
  rpc_request.set_chunk_offset(request.chunk_offset);
  rpc_request.set_chunk_size(request.chunk_size);
  rpc_request.set_rdma_token(request.rdma_token);
  for (const auto& [key, value] : request.extra_headers) {
    (*rpc_request.mutable_extra_headers())[key] = value;
  }

  us3_turbo::proxy::GdsChunkResponse rpc_response;
  stub_->GdsPut(&controller, &rpc_request, &rpc_response, nullptr);

  auto status = CheckRpcFailure(controller, "Failed to execute GDS chunk RPC",
                                request.data_flow, request.request_id);
  if (!status.success()) {
    return Result<us3_turbo::proxy::GdsChunkResponse>::Failure(status.error());
  }
  return Result<us3_turbo::proxy::GdsChunkResponse>::Success(std::move(rpc_response));
}

}  // namespace us3_turbo::client
