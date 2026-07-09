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
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

namespace {

/* block 大小 4MB（与 s3proxy 对齐），part 固定 16MB = 4 blocks */
constexpr std::uint64_t kBlockSize     = 4ULL * 1024 * 1024;   // 4MB
constexpr std::uint64_t kPartSize      = 16ULL * 1024 * 1024;  // 16MB
constexpr std::uint32_t kBlocksPerPart = 4;                    // 16MB / 4MB

/*
 * 生成 block key：{obj_id}_{全局块号}
 */
std::string GenerateBlockKey(const std::string& obj_id,
                             std::uint32_t global_block_index) {
  return obj_id + "_" + std::to_string(global_block_index);
}

/*
 * 尽力清理已写入的 blocks（写中途失败时调用，避免孤儿数据）。
 * 逐块 DeleteBlock，忽略失败（ufile-ac TTL 兜底）。
 */
void CleanupWrittenBlocks(const std::string& req_id,
                          UfileAcClient* client,
                          const std::vector<std::string>& written_keys) {
  if (written_keys.empty()) return;
  LOG_WARN(req_id, "cleaning {} written blocks after failure",
           written_keys.size());
  for (const std::string& key : written_keys) {
    const BlockResult del = client->DeleteBlock(key);
    if (del.ret_code != 0) {
      LOG_DEBUG(req_id, "cleanup skip key={} err={}", key, del.error);
    }
  }
}

}  // namespace

Multipart::Multipart(IUploadIndex* index, UfileAcClient* client)
    : index_(index), client_(client) {}

int Multipart::CreateUpload(
    const std::string& req_id,
    const std::string& bucket, const std::string& key,
    PutDataPath path,
    std::string& out_upload_id) {
  if (bucket.empty() || key.empty()) {
    LOG_WARN(req_id, "bucket/key empty bucket={} key={}", bucket, key);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (path != PATH_GDS && path != PATH_UCX) {
    LOG_WARN(req_id, "path={} not GDS/UCX bucket={}/{}", static_cast<int>(path), bucket, key);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  out_upload_id = index_->Create(bucket, key, path);
  LOG_INFO(req_id, "created upload_id={} bucket={}/{} path={}",
           out_upload_id, bucket, key, static_cast<int>(path));
  return 0;
}

int Multipart::UploadPartGds(
    const std::string& req_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    const std::string& rdma_token,
    UploadPartOutput& out) {
  /* 1. 读取 upload 元信息（需要 obj_id） */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(req_id, "UploadPartGds upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (upload.path != PATH_GDS) {
    LOG_WARN(req_id, "UploadPartGds session path={} != PATH_GDS upload={}",
             static_cast<int>(upload.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }

  /* 2. 基础校验（part_size 上限 16MB，是否最后 part 留到 Complete 判断） */
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(req_id, "UploadPartGds upload={} part={} part_size={} zero",
             upload_id, part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (part_size > kPartSize) {
    LOG_WARN(req_id, "UploadPartGds upload={} part={} size={} > max {}",
             upload_id, part_number, part_size, kPartSize);
    return PROXY_ERR_INVALID_PART_SIZE;
  }
  if (rdma_token.empty()) {
    LOG_WARN(req_id, "UploadPartGds upload={} part={} rdma_token empty",
             upload_id, part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }

  /* 3. 计算全局 block 起点 + 文件偏移（16MB 对齐 → part_number 直接算） */
  const std::uint32_t global_block_start = (part_number - 1) * kBlocksPerPart;
  const std::uint64_t file_offset =
      static_cast<std::uint64_t>(part_number - 1) * kPartSize;
  const std::uint64_t block_count = (part_size + kBlockSize - 1) / kBlockSize;

  LOG_INFO(req_id, "upload={} part={} size={} blocks={} offset={}",
           upload_id, part_number, part_size, block_count, file_offset);

  /* 4. 串行写 blocks，每块成功后立即写索引 */
  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);
  std::vector<std::string> written_keys;
  written_keys.reserve(block_count);

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * kBlockSize;
    const std::uint64_t len = std::min(kBlockSize, part_size - offset);
    const std::string block_key = GenerateBlockKey(
        upload.obj_id, global_block_start + static_cast<std::uint32_t>(i));

    /* block_key 长度守卫（兜底，正常 obj_id_块号 恒 ≤48） */
    if (block_key.size() > KEY_MAX_LENGTH) {
      LOG_ERROR(req_id, "upload={} part={} block_key too long: {} > {}",
                upload_id, part_number, block_key.size(), KEY_MAX_LENGTH);
      CleanupWrittenBlocks(req_id, client_, written_keys);
      return PROXY_ERR_INVALID_PARAM;
    }

    const auto result = client_->PutBlockGds(block_key, rdma_token, offset, len);
    if (result.ret_code != 0) {
      LOG_ERROR(req_id, "upload={} part={} block {} failed: {}",
                upload_id, part_number, i, result.error);
      CleanupWrittenBlocks(req_id, client_, written_keys);
      return result.ret_code;
    }

    written_keys.push_back(block_key);
    crcs.push_back(result.crc32c);

    LOG_DEBUG(req_id, "upload={} part={} block {} ok key={} crc={:#x}",
              upload_id, part_number, i, block_key, result.crc32c);
  }

  /* 5. 计算 part etag */
  const std::string part_etag = utils::CombineBlockCRC32s(crcs);

  /* 6. 写 part 索引（对齐 s3proxy 字段，含 block_crcs 用于对象内容哈希） */
  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = part_etag;
  part.upload_time_ms = utils::NowMs();
  part.file_offset    = file_offset;  // s3proxy 对齐
  part.valid          = true;         // s3proxy 对齐
  part.unmerge_size   = 0;            // s3proxy 对齐（全合并）
  part.block_crcs     = crcs;         // 有序 block crcs，CompleteUpload 拼接算对象哈希
  if (!index_->AddPart(upload_id, part)) {
    LOG_ERROR(req_id, "upload={} part={} AddPart failed", upload_id, part_number);
    CleanupWrittenBlocks(req_id, client_, written_keys);
    return PROXY_ERR_INTERNAL;
  }

  /* 7. 更新 upload 级合并进度（对齐 s3proxy） */
  index_->UpdateMergedSize(upload_id, file_offset + part_size);
  index_->UpdateLastMergedPart(upload_id, static_cast<std::int32_t>(part_number));

  out.etag          = part_etag;
  out.crc32c        = crcs.empty() ? 0 : crcs[0];
  out.bytes_written = part_size;
  LOG_INFO(req_id, "upload={} part={} ok etag={} blocks={}",
           upload_id, part_number, out.etag, crcs.size());
  return 0;
}

int Multipart::UploadPartUcx(
    const std::string& req_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t remote_addr, const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    UploadPartOutput& out) {
  /* 1. 读取 upload 元信息（需要 obj_id） */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(req_id, "UploadPartUcx upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (upload.path != PATH_UCX) {
    LOG_WARN(req_id, "UploadPartUcx session path={} != PATH_UCX upload={}",
             static_cast<int>(upload.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }

  /* 2. 基础校验（part_size 上限 16MB，是否最后 part 留到 Complete 判断） */
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(req_id, "UploadPartUcx upload={} part={} part_size={} zero",
             upload_id, part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (part_size > kPartSize) {
    LOG_WARN(req_id, "UploadPartUcx upload={} part={} size={} > max {}",
             upload_id, part_number, part_size, kPartSize);
    return PROXY_ERR_INVALID_PART_SIZE;
  }
  if (remote_addr == 0 || packed_rkey.empty() || client_ucx_addr.empty()) {
    LOG_WARN(req_id, "UploadPartUcx upload={} part={} ucx source fields incomplete",
             upload_id, part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }

  /* 3. 计算全局 block 起点 + 文件偏移（16MB 对齐 → part_number 直接算） */
  const std::uint32_t global_block_start = (part_number - 1) * kBlocksPerPart;
  const std::uint64_t file_offset =
      static_cast<std::uint64_t>(part_number - 1) * kPartSize;
  const std::uint64_t block_count = (part_size + kBlockSize - 1) / kBlockSize;

  LOG_INFO(req_id, "upload={} part={} size={} blocks={} offset={}",
           upload_id, part_number, part_size, block_count, file_offset);

  /* 4. 串行写 blocks（UCX：remote_addr 基址 + source_offset 偏移），每块成功后立即写索引 */
  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);
  std::vector<std::string> written_keys;
  written_keys.reserve(block_count);

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * kBlockSize;
    const std::uint64_t len = std::min(kBlockSize, part_size - offset);
    const std::string block_key = GenerateBlockKey(
        upload.obj_id, global_block_start + static_cast<std::uint32_t>(i));

    /* block_key 长度守卫（兜底） */
    if (block_key.size() > KEY_MAX_LENGTH) {
      LOG_ERROR(req_id, "upload={} part={} block_key too long: {} > {}",
                upload_id, part_number, block_key.size(), KEY_MAX_LENGTH);
      CleanupWrittenBlocks(req_id, client_, written_keys);
      return PROXY_ERR_INVALID_PARAM;
    }

    const auto result = client_->PutBlockUcx(block_key, remote_addr, packed_rkey,
                                             client_ucx_addr, offset, len);
    if (result.ret_code != 0) {
      LOG_ERROR(req_id, "upload={} part={} block {} failed: {}",
                upload_id, part_number, i, result.error);
      CleanupWrittenBlocks(req_id, client_, written_keys);
      return result.ret_code;
    }

    written_keys.push_back(block_key);
    crcs.push_back(result.crc32c);

    LOG_DEBUG(req_id, "upload={} part={} block {} ok key={} crc={:#x}",
              upload_id, part_number, i, block_key, result.crc32c);
  }

  /* 5. 计算 part etag */
  const std::string part_etag = utils::CombineBlockCRC32s(crcs);

  /* 6. 写 part 索引（对齐 s3proxy 字段，含 block_crcs 用于对象内容哈希） */
  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = part_etag;
  part.upload_time_ms = utils::NowMs();
  part.file_offset    = file_offset;  // s3proxy 对齐
  part.valid          = true;         // s3proxy 对齐
  part.unmerge_size   = 0;            // s3proxy 对齐（全合并）
  part.block_crcs     = crcs;         // 有序 block crcs，CompleteUpload 拼接算对象哈希
  if (!index_->AddPart(upload_id, part)) {
    LOG_ERROR(req_id, "upload={} part={} AddPart failed", upload_id, part_number);
    CleanupWrittenBlocks(req_id, client_, written_keys);
    return PROXY_ERR_INTERNAL;
  }

  /* 7. 更新 upload 级合并进度（对齐 s3proxy） */
  index_->UpdateMergedSize(upload_id, file_offset + part_size);
  index_->UpdateLastMergedPart(upload_id, static_cast<std::int32_t>(part_number));

  out.etag          = part_etag;
  out.crc32c        = crcs.empty() ? 0 : crcs[0];
  out.bytes_written = part_size;
  LOG_INFO(req_id, "upload={} part={} ok etag={} blocks={}",
           upload_id, part_number, out.etag, crcs.size());
  return 0;
}

int Multipart::CompleteUpload(
    const std::string& req_id,
    const std::string& upload_id,
    const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts,
    CompleteOutput& out) {
  /* 1. 读取 upload 元信息 */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(req_id, "upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }

  std::vector<PartRecord> parts;
  index_->ListParts(upload_id, parts);

  /* 2. 按 part_number 升序排序 */
  std::sort(parts.begin(), parts.end(),
            [](const PartRecord& a, const PartRecord& b) {
              return a.part_number < b.part_number;
            });

  /* 3. 校验升序无重复（允许间隙如 1,3,5） */
  int ret = ValidateParts(req_id, parts);
  if (ret != 0) return ret;

  /* 4. 校验所有 part valid（阶段二写入时置 true，兜底） */
  for (const auto& p : parts) {
    if (!p.valid) {
      LOG_WARN(req_id, "upload={} part {} not valid", upload_id, p.part_number);
      return PROXY_ERR_INVALID_PART;
    }
  }

  /* 5. 16MB 对齐校验（除最后一个外必须 16MB） */
  ret = ValidatePartSizes(req_id, parts);
  if (ret != 0) return ret;

  /* 6. client 提供 part 列表时校验 etag 匹配 */
  if (!client_parts.empty()) {
    if (client_parts.size() != parts.size()) {
      LOG_WARN(req_id, "upload={} client parts={} != actual={}",
               upload_id, client_parts.size(), parts.size());
      return PROXY_ERR_INVALID_PARAM;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (client_parts[i].part_number() != parts[i].part_number ||
          client_parts[i].etag() != parts[i].etag) {
        LOG_WARN(req_id, "upload={} part {} etag mismatch",
                 upload_id, parts[i].part_number);
        return PROXY_ERR_INVALID_PARAM;
      }
    }
  }

  /*
   * 7. 使用索引累积的总大小（upload.merged_size 在每个 UploadPart 成功后更新），
   * 并与 parts 求和交叉校验，确保增量索引与 part 记录一致。
   */
  std::uint64_t parts_sum = 0;
  for (const auto& p : parts) parts_sum += p.part_size;
  if (upload.merged_size != parts_sum) {
    LOG_WARN(req_id, "upload={} merged_size={} != parts_sum={} (index inconsistent)",
             upload_id, upload.merged_size, parts_sum);
    return PROXY_ERR_INTERNAL;
  }
  const std::uint64_t total_size = upload.merged_size;

  /*
   * 8. 从已排序的 parts 重建全局有序的 block crcs（= s3proxy US3Etags）。
   * 每个 part 的 block_crcs 写入时即有序，parts 已按 part_number 排序，
   * 故顺序拼接即得全局块号连续的 crc 列表，用于对象内容哈希。
   */
  std::vector<std::uint32_t> object_crcs;
  for (const auto& p : parts) {
    object_crcs.insert(object_crcs.end(), p.block_crcs.begin(), p.block_crcs.end());
  }
  const std::string object_hash = utils::CombineBlockCRC32s(object_crcs);

  /*
   * 9. 零数据操作完成。
   * 数据已在 ufile-ac，key = {obj_id}_{全局块号}，全局连续。
   * 只需生成兼容 s3proxy 的对象元数据（fileidx）。
   * 当前阶段：内存构造 + 日志占位，不写真实 DB（TODO: 对接 MongoDB fileidx 表）。
   * fileidx 关键字段（s3proxy GetObject 依赖）：
   *   first_object = obj_id  → block key 前缀
   *   block_size   = 4MB
   *   filesize     = total_size
   *   hash         = object_hash（US3Etags 组合，s3proxy fileidx.Hash）
   * s3proxy 读取时：block key = first_object + "_" + (offset/block_size)
   */
  const std::string final_etag = ComputeFinalETag(parts);

  LOG_INFO(req_id, "fileidx(compat s3proxy): bucket={} key={} "
           "first_object={} block_size={} filesize={} etag={} hash={}",
           upload.bucket, upload.key, upload.obj_id,
           upload.block_size, total_size, final_etag, object_hash);
  // TODO(stage-db): object_index_->InsertFileIdx({bucket, key, obj_id,
  //                 block_size, total_size, final_etag, object_hash});

  out.object_id   = upload.bucket + "/" + upload.key;
  out.etag        = final_etag;
  out.object_size = total_size;

  /* 9. 清理 upload 索引（成功后） */
  index_->Remove(upload_id);

  LOG_INFO(req_id, "completed object_id={} size={} etag={} parts={}",
           out.object_id, out.object_size, out.etag, parts.size());
  return 0;
}

bool Multipart::AbortUpload(const std::string& req_id,
                            const std::string& upload_id) {
  index_->Remove(upload_id);  // 幂等，不存在也 ok
  LOG_INFO(req_id, "aborted upload={}", upload_id);
  return true;
}

// s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
// parts 已在 CompleteUpload 里按 part_number 排序，此处只校验严格升序。
int Multipart::ValidateParts(const std::string& req_id,
                              const std::vector<PartRecord>& parts) {
  if (parts.empty()) {
    LOG_WARN(req_id, "no parts uploaded");
    return PROXY_ERR_INVALID_PARAM;
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].part_number <= parts[i - 1].part_number) {
      LOG_WARN(req_id, "part {} not strictly ascending at index {}",
               parts[i].part_number, i);
      return PROXY_ERR_INVALID_PARAM;
    }
  }
  return 0;
}

/*
 * 16MB 对齐校验：parts 已按 part_number 升序排列。
 * 规则：除最后一个（最大 part_number）外，所有 part 必须 == 16MB；
 *       最后一个可以 ≤ 16MB（允许不足一个 part）。
 * 这是保证 block key 全局连续、s3proxy 可读的前提。
 */
int Multipart::ValidatePartSizes(const std::string& req_id,
                                 const std::vector<PartRecord>& parts) {
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool is_last = (i == parts.size() - 1);
    if (!is_last && parts[i].part_size != kPartSize) {
      LOG_WARN(req_id, "part {} size {} != required {} (only last part may differ)",
               parts[i].part_number, parts[i].part_size, kPartSize);
      return PROXY_ERR_INVALID_PART_SIZE;
    }
    if (is_last && parts[i].part_size > kPartSize) {
      LOG_WARN(req_id, "last part {} size {} > {}",
               parts[i].part_number, parts[i].part_size, kPartSize);
      return PROXY_ERR_INVALID_PART_SIZE;
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
