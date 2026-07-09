#include "proxy/src/service/single_put.h"

#include <string>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/ufile_ac_client.h"

namespace us3_turbo::proxy {

int SinglePut::ValidateGdsRequest(const ClientProxyPutRequest& req) {
  const std::string& rid = req.request_id();
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.object_size() == 0 ||
      req.object_size() > static_cast<std::uint64_t>(FLAGS_max_single_put_bytes)) {
    LOG_WARN(rid, "object_size={} out of range [1, {}] bucket={}/{}",
             req.object_size(), FLAGS_max_single_put_bytes,
             req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.path() != PATH_GDS) {
    LOG_WARN(rid, "path={} != PATH_GDS", static_cast<int>(req.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!req.has_gds_source()) {
    LOG_WARN(rid, "gds_source missing bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  if (req.gds_source().rdma_token().empty()) {
    LOG_WARN(rid, "gds rdma_token empty bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  return 0;
}

int SinglePut::ValidateUcxRequest(const ClientProxyPutRequest& req) {
  const std::string& rid = req.request_id();
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.object_size() == 0 ||
      req.object_size() > static_cast<std::uint64_t>(FLAGS_max_single_put_bytes)) {
    LOG_WARN(rid, "object_size={} out of range [1, {}] bucket={}/{}",
             req.object_size(), FLAGS_max_single_put_bytes,
             req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.path() != PATH_UCX) {
    LOG_WARN(rid, "path={} != PATH_UCX", static_cast<int>(req.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!req.has_ucx_source()) {
    LOG_WARN(rid, "ucx_source missing bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  const auto& usrc = req.ucx_source();
  if (usrc.remote_addr() == 0 || usrc.packed_rkey().empty() ||
      usrc.client_ucx_addr().empty()) {
    LOG_WARN(rid, "ucx source fields incomplete bucket={}/{}",
             req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  return 0;
}

int SinglePut::PutGds(const ClientProxyPutRequest& req, PutOutput& out) {
  const std::string& rid = req.request_id();

  // ① 校验
  int ret = ValidateGdsRequest(req);
  if (ret != 0) return ret;

  // ② 生成对象标识 + 写单块（key = {obj_id}_0，s3proxy 可读）
  const std::string obj_id     = utils::GenUuid();
  const std::string block_key  = obj_id + "_0";
  const auto result = client_->PutBlockGds(block_key, req.gds_source().rdma_token(),
                                           0, req.object_size());
  if (result.ret_code != 0) {
    LOG_ERROR(rid, "ufile-ac failed: {}", result.error);
    return result.ret_code;
  }
  LOG_DEBUG(rid, "backend ok key={} crc={:#x} bytes={}",
            block_key, result.crc32c, result.bytes_written);

  // ③ 写对象索引 + 填充输出
  WriteObjectIndex(rid, req.bucket(), req.key(), obj_id,
                   req.object_size(), result.crc32c, out);
  return 0;
}

int SinglePut::PutUcx(const ClientProxyPutRequest& req, PutOutput& out) {
  const std::string& rid = req.request_id();

  // ① 校验
  int ret = ValidateUcxRequest(req);
  if (ret != 0) return ret;

  // ② 生成对象标识 + 写单块（key = {obj_id}_0，s3proxy 可读）
  const std::string obj_id     = utils::GenUuid();
  const std::string block_key  = obj_id + "_0";
  const auto& usrc = req.ucx_source();
  const auto result = client_->PutBlockUcx(block_key, usrc.remote_addr(),
                                           usrc.packed_rkey(), usrc.client_ucx_addr(),
                                           0, req.object_size());
  if (result.ret_code != 0) {
    LOG_ERROR(rid, "ufile-ac failed: {}", result.error);
    return result.ret_code;
  }
  LOG_DEBUG(rid, "backend ok key={} crc={:#x} bytes={}",
            block_key, result.crc32c, result.bytes_written);

  // ③ 写对象索引 + 填充输出
  WriteObjectIndex(rid, req.bucket(), req.key(), obj_id,
                   req.object_size(), result.crc32c, out);
  return 0;
}

void SinglePut::WriteObjectIndex(
    const std::string& request_id,
    const std::string& bucket, const std::string& key,
    const std::string& obj_id, std::uint64_t object_size,
    std::uint32_t crc32c, PutOutput& out) {
  // 单块：hash == etag == crc 十六进制（等价 CombineBlockCRC32s 单元素分支，
  // 与原单步 etag 行为一致；非 s3proxy SHA1，proxy 零拷贝拿不到原始数据）
  out.etag          = utils::Crc32cToETag(crc32c);
  out.crc32c        = crc32c;
  out.bytes_written = object_size;

  // fileidx 占位（对齐 s3proxy 字段，格式同 multipart CompleteUpload）：
  //   first_object = obj_id       → block key 前缀
  //   block_size   = object_size  → 单块，s3proxy 读时 blockNum = offset/block_size = 0
  //   filesize     = object_size
  //   hash         = etag（单块 crc）
  // 当前阶段：内存构造 + 日志占位，不写真实 DB（TODO: 对接 MongoDB fileidx 表）。
  LOG_INFO(request_id, "fileidx(compat s3proxy): bucket={} key={} "
           "first_object={} block_size={} filesize={} etag={} hash={}",
           bucket, key, obj_id, object_size, object_size, out.etag, out.etag);
  // TODO(stage-db): object_index_->InsertFileIdx({bucket, key, obj_id,
  //                 object_size, object_size, out.etag});
  //                 落库失败时需 client_->DeleteBlock(obj_id + "_0") 回滚已写块。
}

}  // namespace us3_turbo::proxy
