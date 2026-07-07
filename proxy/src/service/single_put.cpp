#include "proxy/src/service/single_put.h"

#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/storage/backend_gateway.h"

namespace us3_turbo::proxy {

namespace {

// 单步 PUT 对象上限：16 MiB。超出应走分段上传。
// 各模块内各自定义同名常量（client/proxy/backend），不跨模块共享头。
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024;

}  // namespace

SinglePut::SinglePut(BackendGateway* gateway) : gateway_(gateway) {}

int SinglePut::PutGds(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  if (request.bucket().empty() || request.key().empty()) {
    spdlog::warn("PutGds: bucket/key empty bucket={} key={}",
                 request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() == 0) {
    spdlog::warn("PutGds: object_size=0 bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() > kMaxUploadBytes) {
    spdlog::warn("PutGds: object_size={} exceeds 16MiB bucket={}/{}",
                 request.object_size(), request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.path() != ::us3_turbo::proxy::PATH_GDS) {
    spdlog::warn("PutGds: path={} != PATH_GDS", static_cast<int>(request.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!request.has_gds_source()) {
    spdlog::warn("PutGds: gds_source missing bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  return gateway_->ForwardGdsPut(request, out);
}

int SinglePut::PutUcx(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  if (request.bucket().empty() || request.key().empty()) {
    spdlog::warn("PutUcx: bucket/key empty bucket={} key={}",
                 request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() == 0) {
    spdlog::warn("PutUcx: object_size=0 bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() > kMaxUploadBytes) {
    spdlog::warn("PutUcx: object_size={} exceeds 16MiB bucket={}/{}",
                 request.object_size(), request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.path() != ::us3_turbo::proxy::PATH_UCX) {
    spdlog::warn("PutUcx: path={} != PATH_UCX", static_cast<int>(request.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!request.has_ucx_source()) {
    spdlog::warn("PutUcx: ucx_source missing bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  return gateway_->ForwardUcxPut(request, out);
}

}  // namespace us3_turbo::proxy
