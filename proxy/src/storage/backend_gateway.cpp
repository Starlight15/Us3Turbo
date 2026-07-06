#include "proxy/src/storage/backend_gateway.h"

#include <string>
#include <utility>

#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

BackendGateway::BackendGateway(const std::string& backend_endpoint,
                               int timeout_ms)
    : timeout_ms_(timeout_ms) {
  if (backend_endpoint.empty()) {
    spdlog::warn("proxy: backend_endpoint empty, single-step PUT will reject as "
                 "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }
  auto channel = std::make_shared<brpc::Channel>();
  brpc::ChannelOptions options;
  options.timeout_ms = timeout_ms_;
  options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
  if (channel->Init(backend_endpoint.c_str(), nullptr, &options) != 0) {
    spdlog::warn("proxy: failed to init backend channel to {}, single-step PUT disabled",
                 backend_endpoint);
    return;
  }
  channel_ = std::move(channel);
  stub_ = std::make_unique<::us3_turbo::proxy::Control_Stub>(channel_.get());
  spdlog::info("proxy: backend forward channel ready at {} (timeout {}ms)",
               backend_endpoint, timeout_ms_);
}

PutResult BackendGateway::ForwardGdsPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request) {
  if (stub_ == nullptr)
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel"),
            {}, 0, 0};
  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub_->GdsPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed())
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_RPC,
            std::string("backend GdsPut failed: ") + bcntl.ErrorText()),
            {}, 0, 0};
  return {ProxyStatus::Ok(), bresp.etag(), bresp.crc32c(),
          bresp.bytes_written()};
}

PutResult BackendGateway::ForwardUcxPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request) {
  if (stub_ == nullptr)
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel"),
            {}, 0, 0};
  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub_->UcxPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed())
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_RPC,
            std::string("backend UcxPut failed: ") + bcntl.ErrorText()),
            {}, 0, 0};
  return {ProxyStatus::Ok(), bresp.etag(), bresp.crc32c(),
          bresp.bytes_written()};
}

}  // namespace us3_turbo::proxy
