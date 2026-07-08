#include "proxy/src/storage/backend_gateway.h"

#include <string>
#include <utility>

#include <brpc/controller.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

BackendGateway::BackendGateway(const std::string& backend_endpoint,
                               int timeout_ms)
    : timeout_ms_(timeout_ms) {
  if (backend_endpoint.empty()) {
    LOG_SYS_WARN("backend_endpoint empty, single-step PUT will reject as "
                 "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }

  const std::size_t pool_size = static_cast<std::size_t>(FLAGS_backend_conn_pool_size);
  channels_.reserve(pool_size);
  stubs_.reserve(pool_size);

  for (std::size_t i = 0; i < pool_size; ++i) {
    auto channel = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions options;
    options.timeout_ms = timeout_ms_;
    options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
    if (channel->Init(backend_endpoint.c_str(), nullptr, &options) != 0) {
      LOG_SYS_WARN("failed to init backend channel #{}, pool incomplete", i);
      return;  // 任何一条失败都不可用
    }
    channels_.push_back(channel);
    stubs_.push_back(std::make_unique<::us3_turbo::proxy::Control_Stub>(channel.get()));
  }

  LOG_SYS_INFO("backend gateway ready: {} channels at {} (timeout {}ms)",
               pool_size, backend_endpoint, timeout_ms_);
}

int BackendGateway::ForwardGdsPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (stubs_.empty()) {
    LOG_WARN(rid, "backend channel pool unavailable");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }
  LOG_DEBUG(rid, "sending to backend bucket={}/{} size={}",
            request.bucket(), request.key(), request.object_size());

  // 轮询取 stub（无锁）
  const std::size_t idx = next_idx_.fetch_add(1, std::memory_order_relaxed) % stubs_.size();
  auto* stub = stubs_[idx].get();

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub->GdsPut(&bcntl, &request, &bresp, nullptr);
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

  if (stubs_.empty()) {
    LOG_WARN(rid, "backend channel pool unavailable");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }
  LOG_DEBUG(rid, "sending to backend bucket={}/{} size={}",
            request.bucket(), request.key(), request.object_size());

  // 轮询取 stub（无锁）
  const std::size_t idx = next_idx_.fetch_add(1, std::memory_order_relaxed) % stubs_.size();
  auto* stub = stubs_[idx].get();

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  ::us3_turbo::proxy::PutPathResult bresp;
  stub->UcxPut(&bcntl, &request, &bresp, nullptr);
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
