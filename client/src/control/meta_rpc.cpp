#include "client/src/control/meta_rpc.h"

#include <string>

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

bool MetaRpc::OpenSession(const PutAttempt& attempt,
                         SessionGrant& grant) const {
  if (!ok()) {
    spdlog::error("OpenSession (req={}): control-plane channel not ready: {}",
                  attempt.request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, attempt.timeout);

  us3_turbo::proxy::OpenSessionRequest rpc_request;
  rpc_request.set_request_id(attempt.request_id);
  rpc_request.set_session_id(attempt.session_id);
  rpc_request.set_bucket(attempt.bucket);
  rpc_request.set_object_key(attempt.key);
  rpc_request.set_op_type("PUT");
  rpc_request.set_data_flow(std::string(ToString(DataFlow::GPUDirect)));
  rpc_request.set_expected_size(attempt.length.value_or(0));
  rpc_request.set_is_multipart_part(false);

  us3_turbo::proxy::OpenSessionResponse resp;
  stub()->OpenSession(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) || (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to open transfer session: {}",
                  is_timeout ? "timeout" : "control-plane",
                  attempt.request_id, controller.ErrorText());
    return false;
  }

  // 防御性校验:proxy 应原样回显 request_id 并复用 client 提供的 session_id。
  // 不一致说明对端实现异常,拒绝继续以免把错误的会话句柄传给数据面。
  if (resp.request_id() != attempt.request_id ||
      resp.session_id() != attempt.session_id) {
    spdlog::error("OpenSession (req={}): response identity mismatch "
                  "(got req={}, ses={}; expected ses={})",
                  attempt.request_id, resp.request_id(), resp.session_id(),
                  attempt.session_id);
    return false;
  }

  grant.ticket = resp.ticket();
  return true;
}

void MetaRpc::AbortSession(const std::string& session_id,
                           std::chrono::milliseconds timeout) const {
  if (!ok()) return;

  brpc::Controller controller;
  ApplyTimeout(controller, timeout);

  us3_turbo::proxy::AbortSessionRequest req;
  req.set_session_id(session_id);
  us3_turbo::proxy::AbortSessionResponse resp;
  stub()->AbortSession(&controller, &req, &resp, nullptr);

  if (controller.Failed()) {
    spdlog::warn("AbortSession best-effort failed: {}", controller.ErrorText());
  }
}

}  // namespace us3_turbo::client
