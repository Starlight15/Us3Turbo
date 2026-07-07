#include "proxy/src/storage/backend_gateway.h"

#include <string>
#include <utility>

#include <brpc/controller.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

BackendGateway::BackendGateway(const std::string& backend_endpoint,
                               int timeout_ms)
    : timeout_ms_(timeout_ms) {
  if (backend_endpoint.empty()) {
    LOG_WARN("-", "backend_endpoint empty, single-step PUT will reject as "
                  "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }
  auto channel = std::make_shared<brpc::Channel>();
  brpc::ChannelOptions options;
  options.timeout_ms = timeout_ms_;
  options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
  if (channel->Init(backend_endpoint.c_str(), nullptr, &options) != 0) {
    LOG_WARN("-", "failed to init backend channel to {}, single-step PUT disabled",
              backend_endpoint);
    return;
  }
  channel_ = std::move(channel);
  stub_ = std::make_unique<::us3_turbo::proxy::Control_Stub>(channel_.get());
  LOG_INFO("-", "backend forward channel ready at {} (timeout {}ms)",
           backend_endpoint, timeout_ms_);
}

int BackendGateway::ForwardGdsPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (stub_ == nullptr) {
    LOG_WARN(rid, "backend channel unavailable");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }
  LOG_DEBUG(rid, "sending to backend bucket={}/{} size={}",
            request.bucket(), request.key(), request.object_size());

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub_->GdsPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed()) {
    LOG_ERROR(rid, "backend GdsPut failed: {}", bcntl.ErrorText());
    return PROXY_ERR_BACKEND_RPC;
  }
  out.etag          = bresp.etag();
  out.crc32c        = bresp.crc32c();
  out.bytes_written = bresp.bytes_written();
  return 0;
}

int BackendGateway::ForwardUcxPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (stub_ == nullptr) {
    LOG_WARN(rid, "backend channel unavailable");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }
  LOG_DEBUG(rid, "sending to backend bucket={}/{} size={}",
            request.bucket(), request.key(), request.object_size());

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub_->UcxPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed()) {
    LOG_ERROR(rid, "backend UcxPut failed: {}", bcntl.ErrorText());
    return PROXY_ERR_BACKEND_RPC;
  }
  out.etag          = bresp.etag();
  out.crc32c        = bresp.crc32c();
  out.bytes_written = bresp.bytes_written();
  return 0;
}

}  // namespace us3_turbo::proxy
