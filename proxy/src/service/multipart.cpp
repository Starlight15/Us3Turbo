#include "proxy/src/service/multipart.h"

#include <algorithm>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/utils.h"

namespace us3_turbo::proxy {

Multipart::Multipart(IUploadIndex* index, BlockStorage* block_storage)
    : index_(index), block_storage_(block_storage) {}

bool Multipart::CreateUpload(
    const std::string& bucket, const std::string& key,
    ::us3_turbo::proxy::PutDataPath path,
    std::string& out_upload_id, ProxyError& err) {
  if (bucket.empty() || key.empty()) {
    spdlog::warn("CreateUpload: bucket/key empty bucket={} key={}", bucket, key);
    err = {PROXY_ERR_INVALID_PARAM, "bucket or key is empty"};
    return false;
  }
  if (path != ::us3_turbo::proxy::PATH_GDS &&
      path != ::us3_turbo::proxy::PATH_UCX) {
    spdlog::warn("CreateUpload: path={} not GDS/UCX bucket={}/{}",
                 static_cast<int>(path), bucket, key);
    err = {PROXY_ERR_PATH_NOT_SUPPORTED, "path must be PATH_GDS or PATH_UCX"};
    return false;
  }
  out_upload_id = index_->Create(bucket, key, path);
  return true;
}

bool Multipart::UploadPartGds(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    const std::string& rdma_token,
    UploadPartOutput& out, ProxyError& err) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    spdlog::warn("UploadPartGds: upload_id not found upload={}", upload_id);
    err = {PROXY_ERR_INVALID_PARAM, "upload_id not found"};
    return false;
  }
  if (rec.path != ::us3_turbo::proxy::PATH_GDS) {
    spdlog::warn("UploadPartGds: session path={} != PATH_GDS upload={}",
                 static_cast<int>(rec.path), upload_id);
    err = {PROXY_ERR_PATH_NOT_SUPPORTED, "session path is not PATH_GDS"};
    return false;
  }
  if (part_number == 0 || part_size == 0) {
    spdlog::warn("UploadPartGds: upload={} part={} part_size={} zero",
                 upload_id, part_number, part_size);
    err = {PROXY_ERR_INVALID_PARAM, "part_number or part_size is zero"};
    return false;
  }
  if (rdma_token.empty()) {
    spdlog::warn("UploadPartGds: upload={} part={} rdma_token empty",
                 upload_id, part_number);
    err = {PROXY_ERR_MISSING_SOURCE, "rdma_token is empty"};
    return false;
  }

  auto r = block_storage_->PutPartGds(request_id, upload_id, part_number,
                                      part_size, rdma_token);
  if (!r.ok) {
    spdlog::warn("UploadPartGds: upload={} part={} block_storage failed: {}",
                 upload_id, part_number, r.error);
    err = {PROXY_ERR_BACKEND_RPC, "UploadPartGds failed: " + r.error};
    return false;
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
  return true;
}

bool Multipart::UploadPartUcx(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t remote_addr, const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    UploadPartOutput& out, ProxyError& err) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    spdlog::warn("UploadPartUcx: upload_id not found upload={}", upload_id);
    err = {PROXY_ERR_INVALID_PARAM, "upload_id not found"};
    return false;
  }
  if (rec.path != ::us3_turbo::proxy::PATH_UCX) {
    spdlog::warn("UploadPartUcx: session path={} != PATH_UCX upload={}",
                 static_cast<int>(rec.path), upload_id);
    err = {PROXY_ERR_PATH_NOT_SUPPORTED, "session path is not PATH_UCX"};
    return false;
  }
  if (part_number == 0 || part_size == 0) {
    spdlog::warn("UploadPartUcx: upload={} part={} part_size={} zero",
                 upload_id, part_number, part_size);
    err = {PROXY_ERR_INVALID_PARAM, "part_number or part_size is zero"};
    return false;
  }
  if (remote_addr == 0 || packed_rkey.empty() || client_ucx_addr.empty()) {
    spdlog::warn("UploadPartUcx: upload={} part={} ucx source fields incomplete",
                 upload_id, part_number);
    err = {PROXY_ERR_MISSING_SOURCE, "ucx source fields incomplete"};
    return false;
  }

  auto r = block_storage_->PutPartUcx(request_id, upload_id, part_number,
                                      part_size, remote_addr, packed_rkey,
                                      client_ucx_addr);
  if (!r.ok) {
    spdlog::warn("UploadPartUcx: upload={} part={} block_storage failed: {}",
                 upload_id, part_number, r.error);
    err = {PROXY_ERR_BACKEND_RPC, "UploadPartUcx failed: " + r.error};
    return false;
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
  return true;
}

bool Multipart::CompleteUpload(
    const std::string& upload_id,
    const std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>& client_parts,
    CompleteOutput& out, ProxyError& err) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    spdlog::warn("CompleteUpload: upload_id not found upload={}", upload_id);
    err = {PROXY_ERR_INVALID_PARAM, "upload_id not found"};
    return false;
  }

  std::vector<PartRecord> parts;
  index_->ListParts(upload_id, parts);

  // 1. 按 part_number 排序（parts 在索引层未排序）。
  std::sort(parts.begin(), parts.end(),
            [](const PartRecord& a, const PartRecord& b) {
              return a.part_number < b.part_number;
            });

  // 2. 升序无重复校验（s3 语义：允许间隙如 1,3,5）。
  if (!ValidateParts(parts, err)) return false;

  // 3. client 提供 part 列表时校验 etag 匹配。
  if (!client_parts.empty()) {
    if (client_parts.size() != parts.size()) {
      spdlog::warn("CompleteUpload: upload={} client parts={} != actual={}",
                   upload_id, client_parts.size(), parts.size());
      err = {PROXY_ERR_INVALID_PARAM, "part count mismatch"};
      return false;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (client_parts[i].part_number() != parts[i].part_number ||
          client_parts[i].etag() != parts[i].etag) {
        spdlog::warn("CompleteUpload: upload={} part {} etag mismatch",
                     upload_id, parts[i].part_number);
        err = {PROXY_ERR_INVALID_PARAM,
               "part etag mismatch at part " +
               std::to_string(parts[i].part_number)};
        return false;
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
  return true;
}

bool Multipart::AbortUpload(const std::string& upload_id) {
  index_->Remove(upload_id);  // 幂等，不存在也 ok
  return true;
}

// s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
// parts 已在 CompleteUpload 里按 part_number 排序，此处只校验严格升序。
bool Multipart::ValidateParts(const std::vector<PartRecord>& parts,
                              ProxyError& err) {
  if (parts.empty()) {
    spdlog::warn("ValidateParts: no parts uploaded");
    err = {PROXY_ERR_INVALID_PARAM, "no parts uploaded"};
    return false;
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].part_number <= parts[i - 1].part_number) {
      spdlog::warn("ValidateParts: part {} not strictly ascending at index {}",
                   parts[i].part_number, i);
      err = {PROXY_ERR_INVALID_PARAM,
             "part_number not strictly ascending at index " +
             std::to_string(i)};
      return false;
    }
  }
  return true;
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
