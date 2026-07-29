#include "proxy/src/service/single_put.h"

#include <string>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/index/upload_index.h"
#include "proxy/src/storage/ufile_ac_client.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::proxy {

int SinglePut::ValidateGdsRequest(const ClientProxyPutRequest& req) {
  const std::string& rid = req.request_id();
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.object_size() == 0 ||
      req.object_size() > static_cast<std::uint64_t>(FLAGS_max_single_put_bytes)) {
    LOG_WARN(rid, "object_size={} out of range [1, {}] bucket={}/{}", req.object_size(),
             FLAGS_max_single_put_bytes, req.bucket(), req.key());
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


int SinglePut::PutGds(const ClientProxyPutRequest& req, PutOutput& out) {
  const std::string& rid = req.request_id();

  /* 校验 */
  int ret = ValidateGdsRequest(req);
  if (ret != 0) return ret;

  /* 生成对象标识 + 写单块 */
  const std::string obj_id = utils::GenUuid();
  const std::string block_key = obj_id + "_0";
  const auto res =
      client_->PutBlockGds(block_key, req.gds_source().rdma_token(), 0, req.object_size());
  if (res.ret_code != 0) {
    LOG_ERROR(rid, "ufile-ac failed: {}", res.error);
    return res.ret_code;
  }
  LOG_DEBUG(rid, "backend ok key={} crc={:#x} bytes={}", block_key, res.crc32c, res.bytes);

  /* 写对象索引 + 填充输出 */
  if (!WriteObjectIndex(rid, req.bucket(), req.key(), obj_id, req.object_size(), res.crc32c, out)) {
    LOG_ERROR(rid, "WriteObjectIndex failed for key={}", req.key());
    return PROXY_ERR_INDEX_FAILED;
  }
  return 0;
}


int SinglePut::ValidateRdmaRequest(const ClientProxyPutRequest& req) {
  const std::string& rid = req.request_id();
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.object_size() == 0 ||
      req.object_size() > static_cast<std::uint64_t>(FLAGS_max_single_put_bytes)) {
    LOG_WARN(rid, "object_size={} out of range [1, {}] bucket={}/{}", req.object_size(),
             FLAGS_max_single_put_bytes, req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.path() != PATH_RDMA) {
    LOG_WARN(rid, "path={} != PATH_RDMA", static_cast<int>(req.path()));
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (!req.has_rdma_source()) {
    LOG_WARN(rid, "rdma_source missing bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  if (req.rdma_source().rdma_token().empty()) {
    LOG_WARN(rid, "rdma token empty bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  return 0;
}


int SinglePut::PutRdma(const ClientProxyPutRequest& req, PutOutput& out) {
  const std::string& rid = req.request_id();

  /* 校验 */
  int ret = ValidateRdmaRequest(req);
  if (ret != 0) return ret;

  /* 生成对象标识 + 写单块 */
  const std::string obj_id = utils::GenUuid();
  const std::string block_key = obj_id + "_0";
  const auto res = client_->PutBlockRdma(block_key, req.rdma_source().rdma_token(),
                                         0, req.object_size());
  if (res.ret_code != 0) {
    LOG_ERROR(rid, "ufile-ac failed: {}", res.error);
    return res.ret_code;
  }
  LOG_DEBUG(rid, "backend ok key={} crc={:#x} bytes={}", block_key, res.crc32c, res.bytes);

  /* 写对象索引 + 填充输出 */
  if (!WriteObjectIndex(rid, req.bucket(), req.key(), obj_id, req.object_size(), res.crc32c, out)) {
    LOG_ERROR(rid, "WriteObjectIndex failed for key={}", req.key());
    return PROXY_ERR_INDEX_FAILED;
  }
  return 0;
}

bool SinglePut::WriteObjectIndex(const std::string& request_id, const std::string& bucket,
                                 const std::string& key, const std::string& obj_id,
                                 std::uint64_t object_size, std::uint32_t crc32c, PutOutput& out) {
  out.etag = utils::Crc32cToETag(crc32c);
  out.crc32c = crc32c;
  out.bytes = object_size;

  const std::string hash = utils::CombineBlockCRC32s({crc32c});  // single block

  LOG_INFO(request_id,
           "fileidx(compat s3proxy): bucket={} key={} "
           "first_object={} block_size={} filesize={} etag={} hash={}",
           bucket, key, obj_id, object_size, object_size, out.etag, hash);

  /* 写 fileidx_col */
  bool success = index_->InsertFileIdx(bucket, key, obj_id,
                                       object_size,  // block_size = 对象大小（单块）
                                       object_size, hash);

  if (!success) {
    LOG_ERROR(request_id, "Failed to write fileidx for key={}, rolling back block={}", key,
              obj_id + "_0");
    /* 回滚: 删除已写块, 失败由 ufile-ac TTL 兜底 */
    const std::string block_key = obj_id + "_0";
    auto result = client_->DeleteBlock(block_key);
    if (result.ret_code != 0) {
      LOG_WARN(request_id, "DeleteBlock failed during rollback: key={} error={}", block_key,
               result.error);
    }
    out.etag.clear();  // 标记失败
    return false;
  }
  return true;
}

}  // namespace us3_turbo::proxy
