#include "proxy/src/service/single_put_service.h"

#include <utility>

#include "proxy/src/common/errors.h"
#include "proxy/src/storage/backend_gateway.h"

namespace us3_turbo::proxy {

namespace {

// 单步 PUT 对象上限：16 MiB。超出应走分段上传。
// 各模块内各自定义同名常量（client/proxy/backend），不跨模块共享头。
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024;

}  // namespace

SinglePutService::SinglePutService(BackendGateway* gateway)
    : gateway_(gateway) {}

PutResult SinglePutService::PutGds(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request) {
  if (request.bucket().empty() || request.key().empty())
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "missing bucket or key"),
            {}, 0, 0};
  if (request.object_size() == 0)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "object_size must be > 0 for GDS PUT"), {}, 0, 0};
  if (request.object_size() > kMaxUploadBytes)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "object_size exceeds 16MiB single-step limit; use multipart"),
            {}, 0, 0};
  if (request.path() != ::us3_turbo::proxy::PATH_GDS)
    return {ProxyStatus::Fail(PROXY_ERR_PATH_NOT_SUPPORTED,
            "GdsPut requires PATH_GDS"), {}, 0, 0};
  if (!request.has_gds_source())
    return {ProxyStatus::Fail(PROXY_ERR_MISSING_SOURCE,
            "GdsPut requires gds_source"), {}, 0, 0};

  return gateway_->ForwardGdsPut(request);
}

PutResult SinglePutService::PutUcx(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request) {
  if (request.bucket().empty() || request.key().empty())
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "missing bucket or key"),
            {}, 0, 0};
  if (request.object_size() == 0)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "object_size must be > 0 for UCX PUT"), {}, 0, 0};
  if (request.object_size() > kMaxUploadBytes)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "object_size exceeds 16MiB single-step limit; use multipart"),
            {}, 0, 0};
  if (request.path() != ::us3_turbo::proxy::PATH_UCX)
    return {ProxyStatus::Fail(PROXY_ERR_PATH_NOT_SUPPORTED,
            "UcxPut requires PATH_UCX"), {}, 0, 0};
  if (!request.has_ucx_source())
    return {ProxyStatus::Fail(PROXY_ERR_MISSING_SOURCE,
            "UcxPut requires ucx_source"), {}, 0, 0};

  return gateway_->ForwardUcxPut(request);
}

}  // namespace us3_turbo::proxy
