#include "proxy/src/service/get_object.h"

#include <algorithm>
#include <functional>  // std::hash
#include <string>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/snowflake.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/index/upload_index.h"
#include "proxy/src/storage/ufile_ac_client.h"
#include "proxy/src/storage/ufile_ac_protocol.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::proxy {

int GetObject::StatObject(const StatObjectRequest& req, StatObjectOutput& out) {
  const std::string rid = std::to_string(CurrentTraceId());  // proxy snowflake trace_id(由 proxy_service handler set)
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }

  FileIdxRecord rec;
  if (!index_->GetFileIdx(req.bucket(), req.key(), rec)) {
    LOG_WARN(rid, "object not found bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }

  out.object_size = rec.filesize;
  out.block_size = rec.block_size;
  out.hash = rec.hash;
  LOG_DEBUG(rid, "stat bucket={}/{} size={} block_size={} hash={}", req.bucket(), req.key(),
            rec.filesize, rec.block_size, rec.hash);
  return 0;
}

int GetObject::ValidateGdsRequest(const ClientProxyGetRequest& req) {
  const std::string rid = std::to_string(CurrentTraceId());  // proxy snowflake trace_id(由 proxy_service handler set)
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.object_size() == 0) {
    LOG_WARN(rid, "object_size=0 bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
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

int GetObject::ValidateRdmaRequest(const ClientProxyGetRequest& req) {
  const std::string rid = std::to_string(CurrentTraceId());  // proxy snowflake trace_id(由 proxy_service handler set)
  if (req.bucket().empty() || req.key().empty()) {
    LOG_WARN(rid, "bucket/key empty bucket={} key={}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (req.object_size() == 0) {
    LOG_WARN(rid, "object_size=0 bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (!req.has_rdma_source()) {
    LOG_WARN(rid, "rdma_source missing bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  if (req.rdma_source().rdma_token().empty()) {
    LOG_WARN(rid, "rdma rdma_token empty bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_MISSING_SOURCE;
  }
  return 0;
}


int GetObject::GetGds(const ClientProxyGetRequest& req, GetOutput& out) {
  const std::string rid = std::to_string(CurrentTraceId());  // proxy snowflake trace_id(由 proxy_service handler set)

  int ret = ValidateGdsRequest(req);
  if (ret != 0) return ret;

  /* 1. 查布局，校验 object_size 与 fileidx 一致。 */
  FileIdxRecord rec;
  if (!index_->GetFileIdx(req.bucket(), req.key(), rec)) {
    LOG_WARN(rid, "object not found bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (rec.filesize != req.object_size()) {
    LOG_WARN(rid, "object_size mismatch: client={} fileidx={} bucket={}/{}", req.object_size(),
             rec.filesize, req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }

  /* 2. 按 block_size 切块串行读。 */
  const std::uint64_t block_size = rec.block_size;
  const std::uint64_t block_count = (rec.filesize + block_size - 1) / block_size;
  const std::string& rdma_token = req.gds_source().rdma_token();
  const std::uint64_t rid_hash = std::hash<std::string>{}(rid);

  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);
  std::uint64_t total_read = 0;

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * block_size;
    const std::uint64_t len = std::min(block_size, rec.filesize - offset);
    const std::string block_key = rec.first_object + "_" + std::to_string(i);

    const auto result = client_->GetBlockGds(block_key, rdma_token, offset, 0, len, rid_hash);
    if (result.ret_code != 0) {
      LOG_ERROR(rid, "block {} key={} read failed: {}", i, block_key, result.error);
      return result.ret_code;
    }
    crcs.push_back(result.crc32c);
    total_read += result.bytes;
    LOG_DEBUG(rid, "block {} key={} ok crc={:#x} bytes={}", i, block_key, result.crc32c,
              result.bytes);
  }

  /* 3. 重组 hash 与 fileidx 比对。 */
  const std::string hash = utils::CombineBlockCRC32s(crcs);
  if (hash != rec.hash) {
    LOG_ERROR(rid, "hash mismatch: computed={} fileidx={} bucket={}/{}", hash, rec.hash,
              req.bucket(), req.key());
    return PROXY_ERR_BACKEND_FAILED;
  }

  out.crc32c = (crcs.size() == 1) ? crcs[0] : 0;  // 单块整对象 crc，多块用 hash
  out.bytes_read = total_read;
  out.hash = hash;
  LOG_INFO(rid, "GetGds ok bucket={}/{} bytes={} blocks={}", req.bucket(), req.key(), total_read,
           block_count);
  return 0;
}

int GetObject::GetRdma(const ClientProxyGetRequest& req, GetOutput& out) {
  const std::string rid = std::to_string(CurrentTraceId());  // proxy snowflake trace_id(由 proxy_service handler set)

  int ret = ValidateRdmaRequest(req);
  if (ret != 0) return ret;

  /* 1. 查布局，校验 object_size 与 fileidx 一致。 */
  FileIdxRecord rec;
  if (!index_->GetFileIdx(req.bucket(), req.key(), rec)) {
    LOG_WARN(rid, "object not found bucket={}/{}", req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (rec.filesize != req.object_size()) {
    LOG_WARN(rid, "object_size mismatch: client={} fileidx={} bucket={}/{}", req.object_size(),
             rec.filesize, req.bucket(), req.key());
    return PROXY_ERR_INVALID_PARAM;
  }

  /* 2. 按 block_size 切块串行读。 */
  const std::uint64_t block_size = rec.block_size;
  const std::uint64_t block_count = (rec.filesize + block_size - 1) / block_size;
  const std::string& rdma_token = req.rdma_source().rdma_token();
  const std::uint64_t rid_hash = std::hash<std::string>{}(rid);

  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);
  std::uint64_t total_read = 0;

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * block_size;
    const std::uint64_t len = std::min(block_size, rec.filesize - offset);
    const std::string block_key = rec.first_object + "_" + std::to_string(i);

    const auto result = client_->GetBlockRdma(block_key, rdma_token, offset, 0, len, rid_hash);
    if (result.ret_code != 0) {
      LOG_ERROR(rid, "block {} key={} read failed: {}", i, block_key, result.error);
      return result.ret_code;
    }
    crcs.push_back(result.crc32c);
    total_read += result.bytes;
    LOG_DEBUG(rid, "block {} key={} ok crc={:#x} bytes={}", i, block_key, result.crc32c,
              result.bytes);
  }

  /* 3. 重组 hash 与 fileidx 比对。 */
  const std::string hash = utils::CombineBlockCRC32s(crcs);
  if (hash != rec.hash) {
    LOG_ERROR(rid, "hash mismatch: computed={} fileidx={} bucket={}/{}", hash, rec.hash,
              req.bucket(), req.key());
    return PROXY_ERR_BACKEND_FAILED;
  }

  out.crc32c = (crcs.size() == 1) ? crcs[0] : 0;
  out.bytes_read = total_read;
  out.hash = hash;
  LOG_INFO(rid, "GetRdma ok bucket={}/{} bytes={} blocks={}", req.bucket(), req.key(), total_read,
           block_count);
  return 0;
}


}  // namespace us3_turbo::proxy
