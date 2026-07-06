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

bool BackendGateway::ForwardGdsPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out, ProxyError& err) {
  if (stub_ == nullptr) {
    spdlog::warn("ForwardGdsPut: backend channel unavailable");
    err = {PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel"};
    return false;
  }
  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub_->GdsPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed()) {
    spdlog::warn("ForwardGdsPut: backend GdsPut failed: {}", bcntl.ErrorText());
    err = {PROXY_ERR_BACKEND_RPC,
           std::string("backend GdsPut failed: ") + bcntl.ErrorText()};
    return false;
  }
  out.etag          = bresp.etag();
  out.crc32c        = bresp.crc32c();
  out.bytes_written = bresp.bytes_written();
  return true;
}

bool BackendGateway::ForwardUcxPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out, ProxyError& err) {
  if (stub_ == nullptr) {
    spdlog::warn("ForwardUcxPut: backend channel unavailable");
    err = {PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel"};
    return false;
  }
  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub_->UcxPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed()) {
    spdlog::warn("ForwardUcxPut: backend UcxPut failed: {}", bcntl.ErrorText());
    err = {PROXY_ERR_BACKEND_RPC,
           std::string("backend UcxPut failed: ") + bcntl.ErrorText()};
    return false;
  }
  out.etag          = bresp.etag();
  out.crc32c        = bresp.crc32c();
  out.bytes_written = bresp.bytes_written();
  return true;
}

}  // namespace us3_turbo::proxy
