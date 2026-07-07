#include "proxy/src/service/single_put.h"

#include "proxy/src/common/errors.h"
#include "proxy/src/logging/logger.h"
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
  const std::string& rid = request.request_id();

  if (request.bucket().empty() || request.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() == 0) {
    LOG_WARN(rid, "object_size=0 bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() > kMaxUploadBytes) {
    LOG_WARN(rid, "object_size={} exceeds 16MiB bucket={}/{}",
             request.object_size(), request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.path() != ::us3_turbo::proxy::PATH_GDS) {
    LOG_WARN(rid, "path={} != PATH_GDS", static_cast<int>(request.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!request.has_gds_source()) {
    LOG_WARN(rid, "gds_source missing bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  LOG_DEBUG(rid, "validated bucket={}/{} size={} path=GDS",
            request.bucket(), request.key(), request.object_size());
  int ret = gateway_->ForwardGdsPut(request, out);
  if (ret != 0) {
    LOG_ERROR(rid, "backend forward failed code={}", ret);
    return ret;
  }
  LOG_DEBUG(rid, "backend returned etag={} bytes={}", out.etag, out.bytes_written);
  return 0;
}

int SinglePut::PutUcx(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (request.bucket().empty() || request.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() == 0) {
    LOG_WARN(rid, "object_size=0 bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() > kMaxUploadBytes) {
    LOG_WARN(rid, "object_size={} exceeds 16MiB bucket={}/{}",
             request.object_size(), request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.path() != ::us3_turbo::proxy::PATH_UCX) {
    LOG_WARN(rid, "path={} != PATH_UCX", static_cast<int>(request.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!request.has_ucx_source()) {
    LOG_WARN(rid, "ucx_source missing bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  LOG_DEBUG(rid, "validated bucket={}/{} size={} path=UCX",
            request.bucket(), request.key(), request.object_size());
  int ret = gateway_->ForwardUcxPut(request, out);
  if (ret != 0) {
    LOG_ERROR(rid, "backend forward failed code={}", ret);
    return ret;
  }
  LOG_DEBUG(rid, "backend returned etag={} bytes={}", out.etag, out.bytes_written);
  return 0;
}

}  // namespace us3_turbo::proxy
