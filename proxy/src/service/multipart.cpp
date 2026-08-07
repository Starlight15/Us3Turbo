#include "proxy/src/service/multipart.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/storage/ufile_ac_client.h"
#include "proxy/src/storage/ufile_ac_protocol.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::proxy {

std::string Multipart::GenerateBlockKey(const std::string& obj_id,
                                        std::uint32_t global_block_index) const {
  return obj_id + "_" + std::to_string(global_block_index);
}

void Multipart::CleanupWrittenBlocks(const std::string& request_id,
                                     const std::vector<std::string>& written_keys) const {
  if (written_keys.empty()) return;
  LOG_WARN(request_id, "cleaning {} written blocks after failure", written_keys.size());
  for (const std::string& key : written_keys) {
    const BlockResult del = client_->DeleteBlock(key);
    if (del.ret_code != 0) {
      LOG_DEBUG(request_id, "cleanup skip key={} err={}", key, del.error);
    }
  }
}

int Multipart::ValidateUploadPartGds(const std::string& request_id, const std::string& upload_id,
                                     std::uint32_t part_number, std::uint64_t part_size,
                                     const std::string& rdma_token, UploadRecord& out_upload) {
  /* 1. 读取 upload 元信息。 */
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

  /* 2. 基础参数校验。 */
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} part_size={} zero", upload_id,
             part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (part_size > static_cast<std::uint64_t>(FLAGS_multipart_part_size)) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} size={} > max {}", upload_id, part_number,
             part_size, FLAGS_multipart_part_size);
    return PROXY_ERR_INVALID_PART_SIZE;
  }
  if (rdma_token.empty()) {
    LOG_WARN(request_id, "UploadPartGds upload={} part={} rdma_token empty", upload_id,
             part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }
  out_upload = upload;
  return 0;
}


int Multipart::ValidateUploadPartRdma(const std::string& request_id, const std::string& upload_id,
                                      std::uint32_t part_number, std::uint64_t part_size,
                                      const std::string& rdma_token, UploadRecord& out_upload) {
  /* 1. 读取 upload 元信息。 */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(request_id, "UploadPartRdma upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (upload.path != PATH_RDMA) {
    LOG_WARN(request_id, "UploadPartRdma session path={} != PATH_RDMA upload={}",
             static_cast<int>(upload.path), upload_id);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }

  /* 2. 基础参数校验。 */
  if (part_number == 0 || part_size == 0) {
    LOG_WARN(request_id, "UploadPartRdma upload={} part={} part_size={} zero", upload_id,
             part_number, part_size);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (part_size > static_cast<std::uint64_t>(FLAGS_multipart_part_size)) {
    LOG_WARN(request_id, "UploadPartRdma upload={} part={} size={} > max {}", upload_id,
             part_number, part_size, FLAGS_multipart_part_size);
    return PROXY_ERR_INVALID_PART_SIZE;
  }
  if (rdma_token.empty()) {
    LOG_WARN(request_id, "UploadPartRdma upload={} part={} rdma_token empty", upload_id,
             part_number);
    return PROXY_ERR_MISSING_SOURCE;
  }
  out_upload = upload;
  return 0;
}

bool Multipart::WritePartIndex(const std::string& request_id, const std::string& upload_id,
                               std::uint32_t part_number, std::uint64_t part_size,
                               std::uint64_t file_offset,
                               const std::vector<std::uint32_t>& block_crcs,
                               UploadPartOutput& out) {
  /* 1. 计算 part etag。 */
  const std::string part_etag = utils::CombineBlockCRC32s(block_crcs);

  /* 2. 写 part 索引。 */
  PartRecord part;
  part.part_number = part_number;
  part.part_size = part_size;
  part.etag = part_etag;
  part.upload_time_ms = utils::NowMs();
  part.file_offset = file_offset;
  part.valid = true;
  part.block_crcs = block_crcs;
  if (!index_->AddPart(upload_id, part)) {
    LOG_ERROR(request_id, "upload={} part={} AddPart failed", upload_id, part_number);
    // caller 负责清理 blocks
    return false;
  }

  /* 3. 更新 upload 合并进度。 */
  index_->UpdateMergedSize(upload_id, file_offset + part_size);
  index_->UpdateLastMergedPart(upload_id, static_cast<std::int32_t>(part_number));

  /* 4. 填充输出。 */
  out.etag = part_etag;
  out.crc32c = block_crcs.empty() ? 0 : block_crcs[0];
  out.bytes = part_size;
  LOG_INFO(request_id, "upload={} part={} ok etag={} blocks={}", upload_id, part_number, out.etag,
           block_crcs.size());
  return true;
}

int Multipart::CreateUpload(const std::string& request_id, const std::string& bucket,
                            const std::string& key, PutDataPath path, std::string& out_upload_id) {
  if (bucket.empty() || key.empty()) {
    LOG_WARN(request_id, "bucket/key empty bucket={} key={}", bucket, key);
    return PROXY_ERR_INVALID_PARAM;
  }
  if (path != PATH_GDS && path != PATH_RDMA) {
    LOG_WARN(request_id, "path={} not GDS/RDMA bucket={}/{}", static_cast<int>(path), bucket,
             key);
    return PROXY_ERR_PATH_NOT_SUPPORTED;
  }
  out_upload_id = index_->Create(bucket, key, path);
  LOG_INFO(request_id, "created upload_id={} bucket={}/{} path={}", out_upload_id, bucket, key,
           static_cast<int>(path));
  return 0;
}

int Multipart::UploadPartGds(const std::string& request_id, const std::string& upload_id,
                             std::uint32_t part_number, std::uint64_t part_size,
                             const std::string& rdma_token, UploadPartOutput& out) {
  const auto t_start = std::chrono::steady_clock::now();
  /* 1. 校验参数。 */
  UploadRecord upload;
  int ret =
      ValidateUploadPartGds(request_id, upload_id, part_number, part_size, rdma_token, upload);
  if (ret != 0) return ret;

  /* 整 part 一次写入。block_key = obj_id + "_" + (part_number-1)，与 GET 对齐。 */
  const std::uint64_t part_size_limit = static_cast<std::uint64_t>(FLAGS_multipart_part_size);
  const std::uint64_t file_offset = static_cast<std::uint64_t>(part_number - 1) * part_size_limit;
  const std::string block_key = GenerateBlockKey(upload.obj_id, part_number - 1);

  LOG_INFO(request_id, "upload={} part={} size={} block_key={} offset={}", upload_id, part_number,
           part_size, block_key, file_offset);

  if (block_key.size() > KEY_MAX_LENGTH) {
    LOG_ERROR(request_id, "upload={} part={} block_key too long: {} > {}", upload_id, part_number,
              block_key.size(), KEY_MAX_LENGTH);
    return PROXY_ERR_INVALID_PARAM;
  }

  /* 2. 写整 part 到 backend。 */
  const auto t_backend_start = std::chrono::steady_clock::now();
  const auto result = client_->PutBlockGds(block_key, rdma_token, /*gpu_offset=*/0, part_size);
  const auto t_backend = std::chrono::steady_clock::now();
  if (result.ret_code != 0) {
    LOG_ERROR(request_id, "upload={} part={} block failed: {}", upload_id, part_number,
              result.error);
    return result.ret_code;
  }
  LOG_DEBUG(request_id, "upload={} part={} block ok key={} crc={:#x}", upload_id, part_number,
            block_key, result.crc32c);

  /* 3. 写索引并填充输出。索引失败回滚已写块。 */
  const std::vector<std::uint32_t> crcs{result.crc32c};
  const bool index_ok =
      WritePartIndex(request_id, upload_id, part_number, part_size, file_offset, crcs, out);
  const auto t_index = std::chrono::steady_clock::now();
  if (!index_ok) {
    LOG_ERROR(request_id, "WritePartIndex failed for upload={} part={}", upload_id, part_number);
    const std::vector<std::string> written_keys{block_key};
    CleanupWrittenBlocks(request_id, written_keys);
    return PROXY_ERR_INDEX_FAILED;
  }
  if (FLAGS_enable_perf_stats) {
    using clk = std::chrono::steady_clock;
    const auto us = [](clk::time_point a, clk::time_point b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    LOG_INFO(request_id, "[perf/proxy] req={} op=UploadPartGds key={} validate_us={} backend_us={} "
             "index_us={} total_us={} bytes={}",
             request_id, block_key, us(t_start, t_backend_start), us(t_backend_start, t_backend),
             us(t_backend, t_index), us(t_start, t_index), part_size);
  }
  return 0;
}


int Multipart::UploadPartRdma(const std::string& request_id, const std::string& upload_id,
                              std::uint32_t part_number, std::uint64_t part_size,
                              const std::string& rdma_token, UploadPartOutput& out) {
  const auto t_start = std::chrono::steady_clock::now();
  /* 1. 校验参数。 */
  UploadRecord upload;
  int ret =
      ValidateUploadPartRdma(request_id, upload_id, part_number, part_size, rdma_token, upload);
  if (ret != 0) return ret;

  /* 整 part 一次写入。block_key = obj_id + "_" + (part_number-1)，与 GET 对齐。 */
  const std::uint64_t part_size_limit = static_cast<std::uint64_t>(FLAGS_multipart_part_size);
  const std::uint64_t file_offset = static_cast<std::uint64_t>(part_number - 1) * part_size_limit;
  const std::string block_key = GenerateBlockKey(upload.obj_id, part_number - 1);

  LOG_INFO(request_id, "upload={} part={} size={} block_key={} offset={}", upload_id, part_number,
           part_size, block_key, file_offset);

  if (block_key.size() > KEY_MAX_LENGTH) {
    LOG_ERROR(request_id, "upload={} part={} block_key too long: {} > {}", upload_id, part_number,
              block_key.size(), KEY_MAX_LENGTH);
    return PROXY_ERR_INVALID_PARAM;
  }

  /* 2. 写整 part 到 backend。 */
  const auto t_backend_start = std::chrono::steady_clock::now();
  const auto result = client_->PutBlockRdma(block_key, rdma_token, /*source_offset=*/0, part_size);
  const auto t_backend = std::chrono::steady_clock::now();
  if (result.ret_code != 0) {
    LOG_ERROR(request_id, "upload={} part={} block failed: {}", upload_id, part_number,
              result.error);
    return result.ret_code;
  }
  LOG_DEBUG(request_id, "upload={} part={} block ok key={} crc={:#x}", upload_id, part_number,
            block_key, result.crc32c);

  /* 3. 写索引并填充输出。索引失败回滚已写块。 */
  const std::vector<std::uint32_t> crcs{result.crc32c};
  const bool index_ok =
      WritePartIndex(request_id, upload_id, part_number, part_size, file_offset, crcs, out);
  const auto t_index = std::chrono::steady_clock::now();
  if (!index_ok) {
    LOG_ERROR(request_id, "WritePartIndex failed for upload={} part={}", upload_id, part_number);
    const std::vector<std::string> written_keys{block_key};
    CleanupWrittenBlocks(request_id, written_keys);
    return PROXY_ERR_INDEX_FAILED;
  }
  if (FLAGS_enable_perf_stats) {
    using clk = std::chrono::steady_clock;
    const auto us = [](clk::time_point a, clk::time_point b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    LOG_INFO(request_id, "[perf/proxy] req={} op=UploadPartRdma key={} validate_us={} backend_us={} "
             "index_us={} total_us={} bytes={}",
             request_id, block_key, us(t_start, t_backend_start), us(t_backend_start, t_backend),
             us(t_backend, t_index), us(t_start, t_index), part_size);
  }
  return 0;
}

int Multipart::CompleteUpload(
    const std::string& request_id, const std::string& upload_id,
    const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts, CompleteOutput& out) {
  /* 1. 读取 upload 元信息。 */
  UploadRecord upload;
  if (!index_->Get(upload_id, upload)) {
    LOG_WARN(request_id, "upload_id not found upload={}", upload_id);
    return PROXY_ERR_INVALID_PARAM;
  }

  std::vector<PartRecord> parts;
  index_->ListParts(upload_id, parts);

  /* 2. 按 part_number 升序排序。 */
  std::sort(parts.begin(), parts.end(),
            [](const PartRecord& a, const PartRecord& b) { return a.part_number < b.part_number; });

  /* 3. 校验升序无重复。 */
  int ret = ValidateParts(request_id, parts);
  if (ret != 0) return ret;

  /* 4. 校验所有 part valid。 */
  for (const auto& p : parts) {
    if (!p.valid) {
      LOG_WARN(request_id, "upload={} part {} not valid", upload_id, p.part_number);
      return PROXY_ERR_INVALID_PART;
    }
  }

  /* 5. part_size 对齐校验。 */
  ret = ValidatePartSizes(request_id, parts);
  if (ret != 0) return ret;

  /* 6. 校验 client part etag 与索引匹配。 */
  if (!client_parts.empty()) {
    if (client_parts.size() != parts.size()) {
      LOG_WARN(request_id, "upload={} client parts={} != actual={}", upload_id, client_parts.size(),
               parts.size());
      return PROXY_ERR_INVALID_PARAM;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (client_parts[i].part_number() != parts[i].part_number ||
          client_parts[i].etag() != parts[i].etag) {
        LOG_WARN(request_id, "upload={} part {} etag mismatch", upload_id, parts[i].part_number);
        return PROXY_ERR_INVALID_PARAM;
      }
    }
  }

  /* 7. 校验 merged_size 与 parts 求和一致。 */
  std::uint64_t parts_sum = 0;
  for (const auto& p : parts) parts_sum += p.part_size;
  if (upload.merged_size != parts_sum) {
    LOG_WARN(request_id, "upload={} merged_size={} != parts_sum={} (index inconsistent)", upload_id,
             upload.merged_size, parts_sum);
    return PROXY_ERR_INTERNAL;
  }
  const std::uint64_t total_size = upload.merged_size;

  /* 8. 重建全局有序 block crcs。 */
  std::vector<std::uint32_t> object_crcs;
  for (const auto& p : parts) {
    if (p.block_crcs.empty()) {
      LOG_ERROR(request_id, "upload={} part={} block_crcs empty, index corrupted", upload_id,
                p.part_number);
      return PROXY_ERR_INTERNAL;
    }
    object_crcs.insert(object_crcs.end(), p.block_crcs.begin(), p.block_crcs.end());
  }
  const std::string object_hash = utils::CombineBlockCRC32s(object_crcs);

  /* 9. 写 fileidx_col 对象元数据。 */
  const std::string final_etag = ComputeFinalETag(parts);

  LOG_INFO(request_id,
           "fileidx(compat s3proxy): bucket={} key={} "
           "first_object={} block_size={} filesize={} etag={} hash={}",
           upload.bucket, upload.key, upload.obj_id,
           static_cast<std::uint64_t>(FLAGS_multipart_part_size), total_size, final_etag,
           object_hash);

  bool success = index_->InsertFileIdx(
      upload.bucket, upload.key, upload.obj_id,
      static_cast<std::uint64_t>(
          FLAGS_multipart_part_size),  // block_size = part_size（1 block/part）
      total_size, object_hash);

  if (!success) {
    LOG_ERROR(request_id, "Failed to write fileidx for upload={}", upload_id);
    return PROXY_ERR_INDEX_FAILED;
  }

  out.object_id = upload.bucket + "/" + upload.key;
  out.etag = final_etag;
  out.object_size = total_size;

  /* 10. 清理 upload 索引。 */
  index_->Remove(upload_id);

  LOG_INFO(request_id, "completed object_id={} size={} etag={} parts={}", out.object_id,
           out.object_size, out.etag, parts.size());
  return 0;
}

bool Multipart::AbortUpload(const std::string& request_id, const std::string& upload_id) {
  index_->Remove(upload_id);  // 幂等，不存在也 ok
  LOG_INFO(request_id, "aborted upload={}", upload_id);
  return true;
}

/*
 * 校验 parts 严格升序无重复（s3 语义允许间隙，但此处不做，简化实现）。
 */
int Multipart::ValidateParts(const std::string& request_id, const std::vector<PartRecord>& parts) {
  if (parts.empty()) {
    LOG_WARN(request_id, "no parts uploaded");
    return PROXY_ERR_INVALID_PARAM;
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].part_number <= parts[i - 1].part_number) {
      LOG_WARN(request_id, "part {} not strictly ascending at index {}", parts[i].part_number, i);
      return PROXY_ERR_INVALID_PARAM;
    }
  }
  return 0;
}

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
      LOG_WARN(request_id, "last part {} size {} > {}", parts[i].part_number, parts[i].part_size,
               part_size_limit);
      return PROXY_ERR_INVALID_PART_SIZE;
    }
  }
  return 0;
}

/*
 * 计算最终 etag：单 part = 该 part etag；多 part = LE(count) + SHA1(所有 etag) + base64。
 */
std::string Multipart::ComputeFinalETag(const std::vector<PartRecord>& parts) {
  std::vector<std::string> etags;
  etags.reserve(parts.size());
  for (const auto& p : parts) etags.push_back(p.etag);
  return utils::CombineETags(etags);
}

}  // namespace us3_turbo::proxy
