#include "proxy/src/service/multipart_service.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/utils.h"

namespace us3_turbo::proxy {

MultipartService::MultipartService(IUploadIndex* index,
                                   BlockStorage* block_storage)
    : index_(index), block_storage_(block_storage) {}

CreateResult MultipartService::CreateUpload(
    const std::string& bucket, const std::string& key,
    ::us3_turbo::proxy::PutDataPath path) {
  if (bucket.empty() || key.empty())
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "bucket or key is empty"),
            {}};
  if (path != ::us3_turbo::proxy::PATH_GDS &&
      path != ::us3_turbo::proxy::PATH_UCX)
    return {ProxyStatus::Fail(PROXY_ERR_PATH_NOT_SUPPORTED,
            "path must be PATH_GDS or PATH_UCX"), {}};

  const std::string upload_id = index_->Create(bucket, key, path);
  return {ProxyStatus::Ok(), upload_id};
}

UploadPartResult MultipartService::UploadPartGds(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    const std::string& rdma_token) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec))
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "upload_id not found"),
            {}, 0, 0};
  if (rec.path != ::us3_turbo::proxy::PATH_GDS)
    return {ProxyStatus::Fail(PROXY_ERR_PATH_NOT_SUPPORTED,
            "session path is not PATH_GDS"), {}, 0, 0};
  if (part_number == 0 || part_size == 0)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "part_number or part_size is zero"), {}, 0, 0};
  if (rdma_token.empty())
    return {ProxyStatus::Fail(PROXY_ERR_MISSING_SOURCE, "rdma_token is empty"),
            {}, 0, 0};

  auto r = block_storage_->PutPartGds(request_id, upload_id, part_number,
                                      part_size, rdma_token);
  if (!r.ok)
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_RPC,
            "UploadPartGds failed: " + r.error), {}, 0, 0};

  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = r.etag;
  part.upload_time_ms = utils::NowMs();
  index_->AddPart(upload_id, part);

  return {ProxyStatus::Ok(), r.etag, r.crc32c, part_size};
}

UploadPartResult MultipartService::UploadPartUcx(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t remote_addr, const std::string& packed_rkey,
    const std::string& client_ucx_addr) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec))
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "upload_id not found"),
            {}, 0, 0};
  if (rec.path != ::us3_turbo::proxy::PATH_UCX)
    return {ProxyStatus::Fail(PROXY_ERR_PATH_NOT_SUPPORTED,
            "session path is not PATH_UCX"), {}, 0, 0};
  if (part_number == 0 || part_size == 0)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "part_number or part_size is zero"), {}, 0, 0};
  if (remote_addr == 0 || packed_rkey.empty() || client_ucx_addr.empty())
    return {ProxyStatus::Fail(PROXY_ERR_MISSING_SOURCE,
            "ucx source fields incomplete"), {}, 0, 0};

  auto r = block_storage_->PutPartUcx(request_id, upload_id, part_number,
                                      part_size, remote_addr, packed_rkey,
                                      client_ucx_addr);
  if (!r.ok)
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_RPC,
            "UploadPartUcx failed: " + r.error), {}, 0, 0};

  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = r.etag;
  part.upload_time_ms = utils::NowMs();
  index_->AddPart(upload_id, part);

  return {ProxyStatus::Ok(), r.etag, r.crc32c, part_size};
}

CompleteResult MultipartService::CompleteUpload(
    const std::string& upload_id,
    const std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>& client_parts) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec))
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "upload_id not found"),
            {}, {}, 0};

  std::vector<PartRecord> parts;
  index_->ListParts(upload_id, parts);

  // 1. 按 part_number 排序（parts 在索引层未排序）。
  std::sort(parts.begin(), parts.end(),
            [](const PartRecord& a, const PartRecord& b) {
              return a.part_number < b.part_number;
            });

  // 2. 升序无重复校验（s3 语义：允许间隙如 1,3,5）。
  if (auto s = ValidateParts(parts); !s.ok()) return {s, {}, {}, 0};

  // 3. client 提供 part 列表时校验 etag 匹配。
  if (!client_parts.empty()) {
    if (client_parts.size() != parts.size())
      return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "part count mismatch"),
              {}, {}, 0};
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (client_parts[i].part_number() != parts[i].part_number ||
          client_parts[i].etag() != parts[i].etag)
        return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
                "part etag mismatch at part " +
                std::to_string(parts[i].part_number)), {}, {}, 0};
    }
  }

  // 4. 生成最终 object_id / etag / size。
  std::uint64_t size = 0;
  for (const auto& p : parts) size += p.part_size;

  CompleteResult r;
  r.status      = ProxyStatus::Ok();
  r.object_id   = rec.bucket + "/" + rec.key;
  r.etag        = ComputeFinalETag(parts);
  r.object_size = size;
  index_->Remove(upload_id);  // 成功后清理
  return r;
}

ProxyStatus MultipartService::AbortUpload(const std::string& upload_id) {
  index_->Remove(upload_id);  // 幂等，不存在也 ok
  return ProxyStatus::Ok();
}

// s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
// parts 已在 CompleteUpload 里按 part_number 排序，此处只校验严格升序。
ProxyStatus MultipartService::ValidateParts(
    const std::vector<PartRecord>& parts) {
  if (parts.empty())
    return ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "no parts uploaded");
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].part_number <= parts[i - 1].part_number)
      return ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
              "part_number not strictly ascending at index " +
              std::to_string(i));
  }
  return ProxyStatus::Ok();
}

// 单 part → 该 part 的 etag；多 part → 4 字节 LE count 前缀 + SHA1(各 etag 拼接) 再 base64。
std::string MultipartService::ComputeFinalETag(
    const std::vector<PartRecord>& parts) {
  std::vector<std::string> etags;
  etags.reserve(parts.size());
  for (const auto& p : parts) etags.push_back(p.etag);
  return utils::CombineETags(etags);
}

}  // namespace us3_turbo::proxy
