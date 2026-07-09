#include "proxy/src/service/single_put.h"

#include <cstdio>
#include <string>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/ufile_ac_client.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

namespace {

// 单步 PUT 对象上限：16 MiB。超出应走分段上传。
// 各模块内各自定义同名常量（client/proxy/backend），不跨模块共享头。
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024;

}  // namespace

SinglePut::SinglePut(UfileAcClient* client) : client_(client) {}

int SinglePut::PutGds(
    const ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (request.bucket().empty() || request.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() == 0 || request.object_size() > kMaxUploadBytes) {
    LOG_WARN(rid, "object_size={} out of range [1, 16MiB] bucket={}/{}",
             request.object_size(), request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.path() != PATH_GDS) {
    LOG_WARN(rid, "path={} != PATH_GDS", static_cast<int>(request.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!request.has_gds_source()) {
    LOG_WARN(rid, "gds_source missing bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  const auto& gsrc = request.gds_source();
  if (gsrc.rdma_token().empty()) {
    LOG_WARN(rid, "gds rdma_token empty bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  LOG_DEBUG(rid, "validated bucket={}/{} size={} path=GDS",
            request.bucket(), request.key(), request.object_size());
  // key 由 proxy 生成（block 级存储标识）；单步上传 gpu_offset=0
  const std::string key = request.bucket() + "/" + request.key();
  // 子阶段3：key 长度守卫（移自 UfileAcClient；backend KEY_MAX_LENGTH=48）
  if (key.size() > KEY_MAX_LENGTH) {
    LOG_WARN(rid, "key too long: {} > {} bucket={}/{}",
             key.size(), KEY_MAX_LENGTH, request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  auto result = client_->PutBlockGds(key, gsrc.rdma_token(), 0, request.object_size());
  if (result.ret_code != 0) {
    LOG_ERROR(rid, "ufile-ac failed: {}", result.error);
    return result.ret_code;
  }
  out.crc32c        = result.crc32c;
  out.bytes_written = result.bytes_written;
  // backend 不返回 etag（etagLen_=0，F7），以 crc32c 十六进制作 etag 占位
  // （utils::Crc32cToETag，非标准 S3 etag）
  out.etag = utils::Crc32cToETag(result.crc32c);
  LOG_DEBUG(rid, "backend ok etag={} bytes={}", out.etag, out.bytes_written);
  return 0;
}

int SinglePut::PutUcx(
    const ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (request.bucket().empty() || request.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.object_size() == 0 || request.object_size() > kMaxUploadBytes) {
    LOG_WARN(rid, "object_size={} out of range [1, 16MiB] bucket={}/{}",
             request.object_size(), request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (request.path() != PATH_UCX) {
    LOG_WARN(rid, "path={} != PATH_UCX", static_cast<int>(request.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!request.has_ucx_source()) {
    LOG_WARN(rid, "ucx_source missing bucket={}/{}", request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  const auto& usrc = request.ucx_source();
  if (usrc.remote_addr() == 0 || usrc.packed_rkey().empty() ||
      usrc.client_ucx_addr().empty()) {
    LOG_WARN(rid, "ucx source fields incomplete bucket={}/{}",
             request.bucket(), request.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  LOG_DEBUG(rid, "validated bucket={}/{} size={} path=UCX",
            request.bucket(), request.key(), request.object_size());
  // key 由 proxy 生成；单步上传 source_offset=0
  const std::string key = request.bucket() + "/" + request.key();
  // 子阶段3：key 长度守卫（移自 UfileAcClient）
  if (key.size() > KEY_MAX_LENGTH) {
    LOG_WARN(rid, "key too long: {} > {} bucket={}/{}",
             key.size(), KEY_MAX_LENGTH, request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  auto result = client_->PutBlockUcx(
      key, usrc.remote_addr(), usrc.packed_rkey(), usrc.client_ucx_addr(),
      0, request.object_size());
  if (result.ret_code != 0) {
    LOG_ERROR(rid, "ufile-ac failed: {}", result.error);
    return result.ret_code;
  }
  out.crc32c        = result.crc32c;
  out.bytes_written = result.bytes_written;
  out.etag = utils::Crc32cToETag(result.crc32c);
  LOG_DEBUG(rid, "backend ok etag={} bytes={}", out.etag, out.bytes_written);
  return 0;
}

}  // namespace us3_turbo::proxy
