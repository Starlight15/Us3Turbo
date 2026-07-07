#include "proxy/src/service/multipart.h"

#include <algorithm>
#include <string>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

Multipart::Multipart(IUploadIndex* index, BlockStorage* block_storage)
    : index_(index), block_storage_(block_storage) {}

int Multipart::CreateUpload(
    const std::string& bucket, const std::string& key,
    ::us3_turbo::proxy::PutDataPath path,
    std::string& out_upload_id) {
  if (bucket.empty() || key.empty()) {
    LOG_WARN("-", "CreateUpload bucket/key empty bucket={} key={}", bucket, key);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (path != ::us3_turbo::proxy::PATH_GDS &&
      path != ::us3_turbo::proxy::PATH_UCX) {
    LOG_WARN("-", "CreateUpload path={} not GDS/UCX bucket={}/{}",
             static_cast<int>(path), bucket, key);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  out_upload_id = index_->Create(bucket, key, path);
  LOG_INFO("-", "CreateUpload upload_id={} bucket={}/{} path={}",
           out_upload_id, bucket, key, static_cast<int>(path));
  return 0;
}

int Multipart::UploadPartGds(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    const std::string& rdma_token,
    UploadPartOutput& out) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    LOG_WARN(request_id, "UploadPartGds upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (rec.path != ::us3_turbo::proxy::PATH_GDS) {
    LOG_WARN(request_id, "UploadPartGds session path={} != PATH_GDS upload={}",
             static_cast<int>(rec.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} part_size={} zero",
             upload_id, part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (rdma_token.empty()) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} rdma_token empty",
             upload_id, part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }

  auto r = block_storage_->PutPartGds(request_id, upload_id, part_number,
                                      part_size, rdma_token);
  if (r.ret_code != 0) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} block_storage failed: {}",
             upload_id, part_number, r.error);
    return r.ret_code;
  }

  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = r.etag;
  part.upload_time_ms = utils::NowMs();
  index_->AddPart(upload_id, part);

  out.etag          = r.etag;
  out.crc32c        = r.crc32c;
  out.bytes_written = part_size;
  LOG_DEBUG(request_id, "UploadPartGds upload={} part={} etag={} bytes={}",
            upload_id, part_number, out.etag, out.bytes_written);
  return 0;
}

int Multipart::UploadPartUcx(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t remote_addr, const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    UploadPartOutput& out) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    LOG_WARN(request_id, "UploadPartUcx upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (rec.path != ::us3_turbo::proxy::PATH_UCX) {
    LOG_WARN(request_id, "UploadPartUcx session path={} != PATH_UCX upload={}",
             static_cast<int>(rec.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(request_id, "UploadPartUcx upload={} part={} part_size={} zero",
             upload_id, part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (remote_addr == 0 || packed_rkey.empty() || client_ucx_addr.empty()) {
    LOG_WARN(request_id, "UploadPartUcx upload={} part={} ucx source fields incomplete",
             upload_id, part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }

  auto r = block_storage_->PutPartUcx(request_id, upload_id, part_number,
                                      part_size, remote_addr, packed_rkey,
                                      client_ucx_addr);
  if (r.ret_code != 0) {
    LOG_WARN(request_id, "UploadPartUcx upload={} part={} block_storage failed: {}",
             upload_id, part_number, r.error);
    return r.ret_code;
  }

  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = r.etag;
  part.upload_time_ms = utils::NowMs();
  index_->AddPart(upload_id, part);

  out.etag          = r.etag;
  out.crc32c        = r.crc32c;
  out.bytes_written = part_size;
  LOG_DEBUG(request_id, "UploadPartUcx upload={} part={} etag={} bytes={}",
            upload_id, part_number, out.etag, out.bytes_written);
  return 0;
}

int Multipart::CompleteUpload(
    const std::string& upload_id,
    const std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>& client_parts,
    CompleteOutput& out) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    LOG_WARN("-", "CompleteUpload upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }

  std::vector<PartRecord> parts;
  index_->ListParts(upload_id, parts);

  // 1. 按 part_number 排序（parts 在索引层未排序）。
  std::sort(parts.begin(), parts.end(),
            [](const PartRecord& a, const PartRecord& b) {
              return a.part_number < b.part_number;
            });

  // 2. 升序无重复校验（s3 语义：允许间隙如 1,3,5）。
  int ret = ValidateParts(parts);
  if (ret != 0) return ret;

  // 3. client 提供 part 列表时校验 etag 匹配。
  if (!client_parts.empty()) {
    if (client_parts.size() != parts.size()) {
      LOG_WARN("-", "CompleteUpload upload={} client parts={} != actual={}",
               upload_id, client_parts.size(), parts.size());
      return PROXY_ERR_INVALID_PARAM;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (client_parts[i].part_number() != parts[i].part_number ||
          client_parts[i].etag() != parts[i].etag) {
        LOG_WARN("-", "CompleteUpload upload={} part {} etag mismatch",
                 upload_id, parts[i].part_number);
        return PROXY_ERR_INVALID_PARAM;
      }
    }
  }

  // 4. 生成最终 object_id / etag / size。
  std::uint64_t size = 0;
  for (const auto& p : parts) size += p.part_size;

  out.object_id   = rec.bucket + "/" + rec.key;
  out.etag        = ComputeFinalETag(parts);
  out.object_size = size;
  index_->Remove(upload_id);  // 成功后清理
  LOG_INFO("-", "CompleteUpload upload={} object_id={} size={} etag={}",
           upload_id, out.object_id, out.object_size, out.etag);
  return 0;
}

bool Multipart::AbortUpload(const std::string& upload_id) {
  index_->Remove(upload_id);  // 幂等，不存在也 ok
  return true;
}

// s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
// parts 已在 CompleteUpload 里按 part_number 排序，此处只校验严格升序。
int Multipart::ValidateParts(const std::vector<PartRecord>& parts) {
  if (parts.empty()) {
    LOG_WARN("-", "ValidateParts no parts uploaded");
    return PROXY_ERR_INVALID_PARAM;
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].part_number <= parts[i - 1].part_number) {
      LOG_WARN("-", "ValidateParts part {} not strictly ascending at index {}",
               parts[i].part_number, i);
      return PROXY_ERR_INVALID_PARAM;
    }
  }
  return 0;
}

// 单 part → 该 part 的 etag；多 part → 4 字节 LE count 前缀 + SHA1(各 etag 拼接) 再 base64。
std::string Multipart::ComputeFinalETag(
    const std::vector<PartRecord>& parts) {
  std::vector<std::string> etags;
  etags.reserve(parts.size());
  for (const auto& p : parts) etags.push_back(p.etag);
  return utils::CombineETags(etags);
}

}  // namespace us3_turbo::proxy
