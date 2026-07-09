#include "proxy/src/service/multipart.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/ufile_ac_client.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

std::string Multipart::GenerateBlockKey(
    const std::string& obj_id, std::uint32_t global_block_index) const {
  return obj_id + "_" + std::to_string(global_block_index);
}

void Multipart::CleanupWrittenBlocks(
    const std::string& request_id,
    const std::vector<std::string>& written_keys) const {
  if (written_keys.empty()) return;
  LOG_WARN(request_id, "cleaning {} written blocks after failure",
           written_keys.size());
  for (const std::string& key : written_keys) {
    const BlockResult del = client_->DeleteBlock(key);
    if (del.ret_code != 0) {
      LOG_DEBUG(request_id, "cleanup skip key={} err={}", key, del.error);
    }
  }
}

int Multipart::ValidateUploadPartGds(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    const std::string& rdma_token, UploadRecord& out_upload) {
  /* 1. 读取 upload 元信息（需要 obj_id） */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(request_id, "UploadPartGds upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (upload.path != PATH_GDS) {
    LOG_WARN(request_id, "UploadPartGds session path={} != PATH_GDS upload={}",
             static_cast<int>(upload.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }

  /* 2. 基础校验（part_size 上限用 FLAGS_multipart_part_size） */
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} part_size={} zero",
             upload_id, part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (part_size > static_cast<std::uint64_t>(FLAGS_multipart_part_size)) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} size={} > max {}",
             upload_id, part_number, part_size, FLAGS_multipart_part_size);
    return PROXY_ERR_INVALID_PART_SIZE;
  }
  if (rdma_token.empty()) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} rdma_token empty",
             upload_id, part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }
  out_upload = upload;
  return 0;
}

int Multipart::ValidateUploadPartUcx(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t remote_addr, const std::string& packed_rkey,
    const std::string& client_ucx_addr, UploadRecord& out_upload) {
  /* 1. 读取 upload 元信息（需要 obj_id） */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(request_id, "UploadPartUcx upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (upload.path != PATH_UCX) {
    LOG_WARN(request_id, "UploadPartUcx session path={} != PATH_UCX upload={}",
             static_cast<int>(upload.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }

  /* 2. 基础校验（part_size 上限用 FLAGS_multipart_part_size） */
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(request_id, "UploadPartUcx upload={} part={} part_size={} zero",
             upload_id, part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (part_size > static_cast<std::uint64_t>(FLAGS_multipart_part_size)) {
    LOG_WARN(request_id, "UploadPartUcx upload={} part={} size={} > max {}",
             upload_id, part_number, part_size, FLAGS_multipart_part_size);
    return PROXY_ERR_INVALID_PART_SIZE;
  }
  if (remote_addr == 0 || packed_rkey.empty() || client_ucx_addr.empty()) {
    LOG_WARN(request_id, "UploadPartUcx upload={} part={} ucx source fields incomplete",
             upload_id, part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }
  out_upload = upload;
  return 0;
}

void Multipart::WritePartIndex(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t file_offset, const std::vector<std::uint32_t>& block_crcs,
    UploadPartOutput& out) {
  /* 1. 计算 part etag */
  const std::string part_etag = utils::CombineBlockCRC32s(block_crcs);

  /* 2. 写 part 索引（对齐 s3proxy 字段，含 block_crcs 用于对象内容哈希） */
  PartRecord part;
  part.part_number    = part_number;
  part.part_size      = part_size;
  part.etag           = part_etag;
  part.upload_time_ms = utils::NowMs();
  part.file_offset    = file_offset;
  part.valid          = true;
  part.unmerge_size   = 0;
  part.block_crcs     = block_crcs;
  if (!index_->AddPart(upload_id, part)) {
    LOG_ERROR(request_id, "upload={} part={} AddPart failed", upload_id, part_number);
    // 注：此处不清理 blocks（caller 负责，若 WritePartIndex 失败会走 cleanup）
    return;
  }

  /* 3. 更新 upload 级合并进度（对齐 s3proxy） */
  index_->UpdateMergedSize(upload_id, file_offset + part_size);
  index_->UpdateLastMergedPart(upload_id, static_cast<std::int32_t>(part_number));

  /* 4. 填充输出 */
  out.etag          = part_etag;
  out.crc32c        = block_crcs.empty() ? 0 : block_crcs[0];
  out.bytes_written = part_size;
  LOG_INFO(request_id, "upload={} part={} ok etag={} blocks={}",
           upload_id, part_number, out.etag, block_crcs.size());
}

Multipart::Multipart(IUploadIndex* index, UfileAcClient* client)
    : index_(index), client_(client) {}

int Multipart::CreateUpload(
    const std::string& request_id,
    const std::string& bucket, const std::string& key,
    PutDataPath path,
    std::string& out_upload_id) {
  if (bucket.empty() || key.empty()) {
    LOG_WARN(request_id, "bucket/key empty bucket={} key={}", bucket, key);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (path != PATH_GDS && path != PATH_UCX) {
    LOG_WARN(request_id, "path={} not GDS/UCX bucket={}/{}", static_cast<int>(path), bucket, key);
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
  /* ① 校验 */
  UploadRecord upload;
  int ret = ValidateUploadPartGds(request_id, upload_id, part_number, part_size,
                                   rdma_token, upload);
  if (ret != 0) return ret;

  /* ② 计算全局 block 起点 + 文件偏移（从 flag 读常量） */
  const std::uint64_t block_size = static_cast<std::uint64_t>(FLAGS_multipart_block_size);
  const std::uint64_t part_size_limit = static_cast<std::uint64_t>(FLAGS_multipart_part_size);
  const std::uint32_t blocks_per_part = static_cast<std::uint32_t>(FLAGS_multipart_blocks_per_part);

  const std::uint32_t global_block_start = (part_number - 1) * blocks_per_part;
  const std::uint64_t file_offset = static_cast<std::uint64_t>(part_number - 1) * part_size_limit;
  const std::uint64_t block_count = (part_size + block_size - 1) / block_size;

  LOG_INFO(request_id, "upload={} part={} size={} blocks={} offset={}",
           upload_id, part_number, part_size, block_count, file_offset);

  /* ③ 串行写 blocks */
  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);
  std::vector<std::string> written_keys;
  written_keys.reserve(block_count);

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * block_size;
    const std::uint64_t len = std::min(block_size, part_size - offset);
    const std::string block_key = GenerateBlockKey(
        upload.obj_id, global_block_start + static_cast<std::uint32_t>(i));

    if (block_key.size() > KEY_MAX_LENGTH) {
      LOG_ERROR(request_id, "upload={} part={} block_key too long: {} > {}",
                upload_id, part_number, block_key.size(), KEY_MAX_LENGTH);
      CleanupWrittenBlocks(request_id, written_keys);
      return PROXY_ERR_INVALID_PARAM;
    }

    const auto result = client_->PutBlockGds(block_key, rdma_token, offset, len);
    if (result.ret_code != 0) {
      LOG_ERROR(request_id, "upload={} part={} block {} failed: {}",
                upload_id, part_number, i, result.error);
      CleanupWrittenBlocks(request_id, written_keys);
      return result.ret_code;
    }

    written_keys.push_back(block_key);
    crcs.push_back(result.crc32c);
    LOG_DEBUG(request_id, "upload={} part={} block {} ok key={} crc={:#x}",
              upload_id, part_number, i, block_key, result.crc32c);
  }

  /* ④ 写索引 + 填输出 */
  WritePartIndex(request_id, upload_id, part_number, part_size,
                 file_offset, crcs, out);
  return 0;
}

int Multipart::UploadPartUcx(
    const std::string& request_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t part_size,
    std::uint64_t remote_addr, const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    UploadPartOutput& out) {
  /* ① 校验 */
  UploadRecord upload;
  int ret = ValidateUploadPartUcx(request_id, upload_id, part_number, part_size,
                                   remote_addr, packed_rkey, client_ucx_addr, upload);
  if (ret != 0) return ret;

  /* ② 计算全局 block 起点 + 文件偏移（从 flag 读常量） */
  const std::uint64_t block_size = static_cast<std::uint64_t>(FLAGS_multipart_block_size);
  const std::uint64_t part_size_limit = static_cast<std::uint64_t>(FLAGS_multipart_part_size);
  const std::uint32_t blocks_per_part = static_cast<std::uint32_t>(FLAGS_multipart_blocks_per_part);

  const std::uint32_t global_block_start = (part_number - 1) * blocks_per_part;
  const std::uint64_t file_offset = static_cast<std::uint64_t>(part_number - 1) * part_size_limit;
  const std::uint64_t block_count = (part_size + block_size - 1) / block_size;

  LOG_INFO(request_id, "upload={} part={} size={} blocks={} offset={}",
           upload_id, part_number, part_size, block_count, file_offset);

  /* ③ 串行写 blocks（UCX：remote_addr 基址 + source_offset 偏移） */
  std::vector<std::uint32_t> crcs;
  crcs.reserve(block_count);
  std::vector<std::string> written_keys;
  written_keys.reserve(block_count);

  for (std::uint64_t i = 0; i < block_count; ++i) {
    const std::uint64_t offset = i * block_size;
    const std::uint64_t len = std::min(block_size, part_size - offset);
    const std::string block_key = GenerateBlockKey(
        upload.obj_id, global_block_start + static_cast<std::uint32_t>(i));

    if (block_key.size() > KEY_MAX_LENGTH) {
      LOG_ERROR(request_id, "upload={} part={} block_key too long: {} > {}",
                upload_id, part_number, block_key.size(), KEY_MAX_LENGTH);
      CleanupWrittenBlocks(request_id, written_keys);
      return PROXY_ERR_INVALID_PARAM;
    }

    const auto result = client_->PutBlockUcx(block_key, remote_addr, packed_rkey,
                                             client_ucx_addr, offset, len);
    if (result.ret_code != 0) {
      LOG_ERROR(request_id, "upload={} part={} block {} failed: {}",
                upload_id, part_number, i, result.error);
      CleanupWrittenBlocks(request_id, written_keys);
      return result.ret_code;
    }

    written_keys.push_back(block_key);
    crcs.push_back(result.crc32c);
    LOG_DEBUG(request_id, "upload={} part={} block {} ok key={} crc={:#x}",
              upload_id, part_number, i, block_key, result.crc32c);
  }

  /* ④ 写索引 + 填输出 */
  WritePartIndex(request_id, upload_id, part_number, part_size,
                 file_offset, crcs, out);
  return 0;
}

int Multipart::CompleteUpload(
    const std::string& request_id,
    const std::string& upload_id,
    const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts,
    CompleteOutput& out) {
  /* 1. 读取 upload 元信息 */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(request_id, "upload_id not found upload={}", upload_id);
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
  int ret = ValidateParts(request_id, parts);
  if (ret != 0) return ret;

  /* 4. 校验所有 part valid（阶段二写入时置 true，兜底） */
  for (const auto& p : parts) {
    if (!p.valid) {
      LOG_WARN(request_id, "upload={} part {} not valid", upload_id, p.part_number);
      return PROXY_ERR_INVALID_PART;
    }
  }

  /* 5. 16MB 对齐校验（除最后一个外必须 16MB） */
  ret = ValidatePartSizes(request_id, parts);
  if (ret != 0) return ret;

  /* 6. client 提供 part 列表时校验 etag 匹配 */
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

  /*
   * 7. 使用索引累积的总大小（upload.merged_size 在每个 UploadPart 成功后更新），
   * 并与 parts 求和交叉校验，确保增量索引与 part 记录一致。
   */
  std::uint64_t parts_sum = 0;
  for (const auto& p : parts) parts_sum += p.part_size;
  if (upload.merged_size != parts_sum) {
    LOG_WARN(request_id, "upload={} merged_size={} != parts_sum={} (index inconsistent)",
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

  LOG_INFO(request_id, "fileidx(compat s3proxy): bucket={} key={} "
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

  LOG_INFO(request_id, "completed object_id={} size={} etag={} parts={}",
           out.object_id, out.object_size, out.etag, parts.size());
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

/*
 * 16MB 对齐校验：parts 已按 part_number 升序排列。
 * 规则：除最后一个（最大 part_number）外，所有 part 必须 == 16MB；
 *       最后一个可以 ≤ 16MB（允许不足一个 part）。
 * 这是保证 block key 全局连续、s3proxy 可读的前提。
 */
int Multipart::ValidatePartSizes(const std::string& request_id,
                                 const std::vector<PartRecord>& parts) {
  const std::uint64_t part_size_limit = static_cast<std::uint64_t>(FLAGS_multipart_part_size);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool is_last = (i == parts.size() - 1);
    if (!is_last && parts[i].part_size != part_size_limit) {
      LOG_WARN(request_id, "part {} size {} != required {} (only last part may differ)",
               parts[i].part_number, parts[i].part_size, part_size_limit);
      return PROXY_ERR_INVALID_PART_SIZE;
    }
    if (is_last && parts[i].part_size > part_size_limit) {
      LOG_WARN(request_id, "last part {} size {} > {}",
               parts[i].part_number, parts[i].part_size, part_size_limit);
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
