#include "proxy/src/service/multipart.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/ufile_ac_client.h"

namespace us3_turbo::proxy {

namespace {

// block 大小：4MB（与 s3proxy 对齐；单 part 超此则拆多块串行写）。
constexpr std::uint64_t kBlockSize = 4ULL * 1024 * 1024;

// 去除 UUID 连字符（36 → 32 字节），压缩 block key 以符合 backend KEY_MAX_LENGTH(48)。
std::string StripUuidDashes(const std::string& upload_id) {
  std::string result;
  result.reserve(32);
  for (char c : upload_id) {
    if (c != '-') result.push_back(c);
  }
  return result;
}

// 生成紧凑 block key：mp/{uuid32}/p{part:04u}b{block:02u}
// 长度 = 3 + 32 + 1 + 5 + 6 = 47 ≤ KEY_MAX_LENGTH(48) ✓
std::string GenerateBlockKey(const std::string& upload_id,
                             std::uint32_t part_number,
                             std::uint32_t block_no) {
  const std::string uuid_nodash = StripUuidDashes(upload_id);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "mp/%s/p%04ub%02u",
                uuid_nodash.c_str(), part_number, block_no);
  return buf;
}

}  // namespace

Multipart::Multipart(IUploadIndex* index, BlockStorage* block_storage)
    : index_(index), block_storage_(block_storage) {}

int Multipart::CreateUpload(
    const std::string& request_id,
    const std::string& bucket, const std::string& key,
    PutDataPath path,
    std::string& out_upload_id) {
  if (bucket.empty() || key.empty()) {
    LOG_WARN(request_id, "bucket/key empty bucket={} key={}", bucket, key);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (path != PATH_GDS &&
      path != PATH_UCX) {
    LOG_WARN(request_id, "path={} not GDS/UCX bucket={}/{}",
             static_cast<int>(path), bucket, key);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  out_upload_id = index_->Create(bucket, key, path);
  LOG_INFO(request_id, "created upload_id={} bucket={}/{} path={}",
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
  if (rec.path != PATH_GDS) {
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

  // 阶段二：part → blocks 串行写 ufile-ac，每块独立 key + crc32c，记录 BlockInfo。
  UfileAcClient* client = block_storage_->GetUfileAcClient();
  if (client == nullptr) {
    LOG_WARN(request_id, "upload={} part={} ufile-ac client unavailable",
             upload_id, part_number);
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }

  const std::uint64_t block_count = (part_size + kBlockSize - 1) / kBlockSize;
  LOG_INFO(request_id, "upload={} part={} size={} blocks={}",
           upload_id, part_number, part_size, block_count);

  std::vector<BlockInfo> block_infos;
  block_infos.reserve(block_count);
  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * kBlockSize;
    const std::uint64_t len = std::min(kBlockSize, part_size - offset);
    const std::string block_key = GenerateBlockKey(
        upload_id, part_number, static_cast<std::uint32_t>(i));

    auto result = client->PutBlockGds(block_key, rdma_token, offset, len);
    if (result.ret_code != 0) {
      LOG_ERROR(request_id, "upload={} part={} block {} failed: {}",
                upload_id, part_number, i, result.error);
      return result.ret_code;  // 已写 block 留 ufile-ac TTL 清理
    }

    BlockInfo info;
    info.key    = block_key;
    info.offset = offset;
    info.size   = len;
    info.crc32c = result.crc32c;
    block_infos.push_back(std::move(info));
    crcs.push_back(result.crc32c);
    LOG_DEBUG(request_id, "upload={} part={} block {} ok key={} crc={:#x}",
              upload_id, part_number, i, block_key, result.crc32c);
  }

  const std::string part_etag = utils::CombineBlockCRC32s(crcs);

  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = part_etag;
  part.upload_time_ms = utils::NowMs();
  part.blocks         = std::move(block_infos);
  index_->AddPart(upload_id, part);

  out.etag          = part_etag;
  out.crc32c        = crcs[0];
  out.bytes_written = part_size;
  LOG_INFO(request_id, "upload={} part={} ok etag={} blocks={}",
           upload_id, part_number, out.etag, part.blocks.size());
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
  if (rec.path != PATH_UCX) {
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

  // 阶段二：part → blocks 串行写 ufile-ac（UCX：remote_addr 基址 + source_offset 偏移）。
  UfileAcClient* client = block_storage_->GetUfileAcClient();
  if (client == nullptr) {
    LOG_WARN(request_id, "upload={} part={} ufile-ac client unavailable",
             upload_id, part_number);
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }

  const std::uint64_t block_count = (part_size + kBlockSize - 1) / kBlockSize;
  LOG_INFO(request_id, "upload={} part={} size={} blocks={}",
           upload_id, part_number, part_size, block_count);

  std::vector<BlockInfo> block_infos;
  block_infos.reserve(block_count);
  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * kBlockSize;
    const std::uint64_t len = std::min(kBlockSize, part_size - offset);
    const std::string block_key = GenerateBlockKey(
        upload_id, part_number, static_cast<std::uint32_t>(i));

    auto result = client->PutBlockUcx(block_key, remote_addr, packed_rkey,
                                      client_ucx_addr, offset, len);
    if (result.ret_code != 0) {
      LOG_ERROR(request_id, "upload={} part={} block {} failed: {}",
                upload_id, part_number, i, result.error);
      return result.ret_code;  // 已写 block 留 ufile-ac TTL 清理
    }

    BlockInfo info;
    info.key    = block_key;
    info.offset = offset;
    info.size   = len;
    info.crc32c = result.crc32c;
    block_infos.push_back(std::move(info));
    crcs.push_back(result.crc32c);
    LOG_DEBUG(request_id, "upload={} part={} block {} ok key={} crc={:#x}",
              upload_id, part_number, i, block_key, result.crc32c);
  }

  const std::string part_etag = utils::CombineBlockCRC32s(crcs);

  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = part_etag;
  part.upload_time_ms = utils::NowMs();
  part.blocks         = std::move(block_infos);
  index_->AddPart(upload_id, part);

  out.etag          = part_etag;
  out.crc32c        = crcs[0];
  out.bytes_written = part_size;
  LOG_INFO(request_id, "upload={} part={} ok etag={} blocks={}",
           upload_id, part_number, out.etag, part.blocks.size());
  return 0;
}

int Multipart::CompleteUpload(
    const std::string& request_id,
    const std::string& upload_id,
    const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts,
    CompleteOutput& out) {
  UploadRecord rec;
  if (!index_->Get(upload_id, rec)) {
    LOG_WARN(request_id, "upload_id not found upload={}", upload_id);
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
  int ret = ValidateParts(request_id, parts);
  if (ret != 0) return ret;

  // 3. client 提供 part 列表时校验 etag 匹配。
  if (!client_parts.empty()) {
    if (client_parts.size() != parts.size()) {
      LOG_WARN(request_id, "upload={} client parts={} != actual={}",
               upload_id, client_parts.size(), parts.size());
      return PROXY_ERR_INVALID_PARAM;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (client_parts[i].part_number() != parts[i].part_number ||
          client_parts[i].etag() != parts[i].etag) {
        LOG_WARN(request_id, "upload={} part {} etag mismatch",
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
  LOG_INFO(request_id, "completed object_id={} size={} etag={}",
           out.object_id, out.object_size, out.etag);
  return 0;
}

bool Multipart::AbortUpload(const std::string& request_id,
                            const std::string& upload_id) {
  index_->Remove(upload_id);  // 幂等，不存在也 ok
  LOG_INFO(request_id, "aborted upload={}", upload_id);
  return true;
}

// s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
// parts 已在 CompleteUpload 里按 part_number 排序，此处只校验严格升序。
int Multipart::ValidateParts(const std::string& request_id,
                              const std::vector<PartRecord>& parts) {
  if (parts.empty()) {
    LOG_WARN(request_id, "no parts uploaded");
    return PROXY_ERR_INVALID_PARAM;
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].part_number <= parts[i - 1].part_number) {
      LOG_WARN(request_id, "part {} not strictly ascending at index {}",
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
