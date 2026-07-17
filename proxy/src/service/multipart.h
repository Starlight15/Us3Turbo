#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"
#include "proxy/src/index/upload_index.h"

namespace us3_turbo::proxy {

class UfileAcClient;

/* 分段上传输出 */
struct UploadPartOutput {
  std::string etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

struct CompleteOutput {
  std::string object_id;
  std::string etag;
  std::uint64_t object_size{0};
};

/* 分段上传逻辑层，编排 Create/UploadPart/Complete/Abort */
class Multipart {
 public:
  Multipart(IUploadIndex* index, UfileAcClient* client)
      : index_(index), client_(client) {}

  [[nodiscard]] int CreateUpload(const std::string& request_id,
                                 const std::string& bucket,
                                 const std::string& key, PutDataPath path,
                                 std::string& out_upload_id);

  [[nodiscard]] int UploadPartGds(const std::string& request_id,
                                  const std::string& upload_id,
                                  std::uint32_t part_number,
                                  std::uint64_t part_size,
                                  const std::string& rdma_token,
                                  UploadPartOutput& out);

  [[nodiscard]] int UploadPartUcx(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t remote_addr, const std::string& packed_rkey,
      const std::string& client_ucx_addr, UploadPartOutput& out);

  [[nodiscard]] int CompleteUpload(
      const std::string& request_id, const std::string& upload_id,
      const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts,
      CompleteOutput& out);

  /* 幂等，恒 true */
  [[nodiscard]] bool AbortUpload(const std::string& request_id,
                                 const std::string& upload_id);

 private:
  /* 生成 block key: {obj_id}_{global_block_index} */
  [[nodiscard]] std::string GenerateBlockKey(
      const std::string& obj_id, std::uint32_t global_block_index) const;

  /* 清理已写 blocks（写中途失败回滚）*/
  void CleanupWrittenBlocks(const std::string& request_id,
                            const std::vector<std::string>& written_keys) const;

  /* UploadPartGds 子阶段: 校验 upload 会话 + part 参数 */
  [[nodiscard]] int ValidateUploadPartGds(const std::string& request_id,
                                          const std::string& upload_id,
                                          std::uint32_t part_number,
                                          std::uint64_t part_size,
                                          const std::string& rdma_token,
                                          UploadRecord& out_upload);

  /* UploadPartUcx 子阶段: 校验 upload 会话 + part 参数 */
  [[nodiscard]] int ValidateUploadPartUcx(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t remote_addr, const std::string& packed_rkey,
      const std::string& client_ucx_addr, UploadRecord& out_upload);

  /* 公共子阶段: 写 part 索引 + 更新 upload 进度 + 填充输出 */
  [[nodiscard]] bool WritePartIndex(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t file_offset, const std::vector<std::uint32_t>& block_crcs,
      UploadPartOutput& out);

  [[nodiscard]] int ValidateParts(const std::string& request_id,
                                  const std::vector<PartRecord>& parts);
  /* part_size 对齐校验：除最后一个 part 外必须 == part_size_limit */
  [[nodiscard]] int ValidatePartSizes(const std::string& request_id,
                                      const std::vector<PartRecord>& parts);
  std::string ComputeFinalETag(const std::vector<PartRecord>& parts);

  IUploadIndex* index_;
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
