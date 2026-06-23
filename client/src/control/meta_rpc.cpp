#include "client/src/control/meta_rpc.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/common/brpc_util.h"
#include "client/src/common/errors.h"

namespace us3_turbo::client {

MetaRpc::MetaRpc(const std::string& endpoint, std::chrono::milliseconds timeout) {
  // channel 由 InitBrpcChannel 统一构建:endpoint 为空或 Init 失败时返回
  // nullptr、init_error_ 存原因,ok() 返回 false。
  channel_ = InitBrpcChannel(endpoint, timeout, init_error_, "control");
  if (!channel_) {
    return;
  }
  stub_ = std::make_unique<us3_turbo::proxy::Control_Stub>(channel_.get());
}

Result<us3_turbo::proxy::OpenSessionResponse>
MetaRpc::OpenSession(const OpenSessionRequest& request) const {
  if (!ok()) {
    return Result<us3_turbo::proxy::OpenSessionResponse>::Failure(
        MakeError(ErrorCode::kRpcError, init_error_, /*retryable=*/true));
  }
  brpc::Controller controller;
  ApplyRequestHeaders(controller, request.context);

  us3_turbo::proxy::OpenSessionRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_bucket(request.bucket);
  rpc_request.set_object_key(request.key);
  rpc_request.set_op_type(std::string(ToString(request.operation)));
  rpc_request.set_data_flow(std::string(ToString(request.data_flow)));
  rpc_request.set_offset(request.offset);
  rpc_request.set_expected_size(request.length.value_or(0));
  rpc_request.set_idempotency_key(request.idempotency_key);
  rpc_request.set_is_multipart_part(request.is_multipart_part);

  us3_turbo::proxy::OpenSessionResponse rpc_response;
  stub_->OpenSession(&controller, &rpc_request, &rpc_response, nullptr);

  auto status = CheckRpcFailure(controller, "Failed to open transfer session",
                                request.data_flow, request.request_id);
  if (!status.success()) {
    return Result<us3_turbo::proxy::OpenSessionResponse>::Failure(status.error());
  }
  return Result<us3_turbo::proxy::OpenSessionResponse>::Success(
      std::move(rpc_response));
}

Result<bool> MetaRpc::AbortSession(const std::string& session_id,
                                   const RpcCallMetadata& context) const {
  if (!ok()) {
    // best-effort:channel 不可用时也按 success(false) 处理,不干扰主流程。
    return Result<bool>::Success(false);
  }
  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  us3_turbo::proxy::AbortSessionRequest req;
  req.set_session_id(session_id);
  us3_turbo::proxy::AbortSessionResponse resp;
  stub_->AbortSession(&controller, &req, &resp, nullptr);
  // best-effort:RPC 失败也算 success(false),不让重试失败干扰主流程。
  if (controller.Failed()) {
    return Result<bool>::Success(false);
  }
  return Result<bool>::Success(resp.erased());
}

}  // namespace us3_turbo::client
