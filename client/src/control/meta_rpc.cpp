#include "client/src/control/meta_rpc.h"

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "control_plane.pb.h"

namespace us3_turbo::client {

bool MetaRpc::OpenSession(const OpenSessionRequest& request,
                         SessionMeta& out) const {
  if (!ok()) {
    spdlog::error("OpenSession (req={}): control-plane channel not ready: {}",
                  request.request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, request.timeout);

  us3_turbo::proxy::OpenSessionRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_bucket(request.bucket);
  rpc_request.set_object_key(request.key);
  rpc_request.set_op_type("PUT");
  rpc_request.set_data_flow(std::string(ToString(DataFlow::GPUDirect)));
  rpc_request.set_expected_size(request.length.value_or(0));
  rpc_request.set_is_multipart_part(false);

  us3_turbo::proxy::OpenSessionResponse resp;
  stub()->OpenSession(&controller, &rpc_request, &resp, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) || (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to open transfer session: {}",
                  is_timeout ? "timeout" : "control-plane",
                  request.request_id, controller.ErrorText());
    return false;
  }

  out.request_id = resp.request_id();
  out.session_id = resp.session_id();
  out.ticket     = resp.ticket();
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
