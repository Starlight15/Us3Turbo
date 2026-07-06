#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control_plane.pb.h"
#include "proxy/src/common/status.h"
#include "proxy/src/index/upload_index.h"
#include "proxy/src/storage/block_storage.h"

namespace us3_turbo::proxy {

// 分段上传服务层结果类型。
struct CreateResult {
  ProxyStatus  status;
  std::string  upload_id;
};

struct UploadPartResult {
  ProxyStatus   status;
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

struct CompleteResult {
  ProxyStatus   status;
  std::string   object_id;
  std::string   etag;
  std::uint64_t object_size{0};
};

// 分段上传服务：编排 Create/UploadPart/Complete/Abort，业务规则（part 校验 /
// final etag / client etag 比对）在本类。GDS/UCX 各自独立方法。依赖
// IUploadIndex*（mock/Mongo 无差别替换）+ BlockStorage*（block 切分转发）。
class MultipartService {
 public:
  MultipartService(IUploadIndex* index, BlockStorage* block_storage);

  [[nodiscard]] CreateResult CreateUpload(
      const std::string& bucket, const std::string& key,
      ::us3_turbo::proxy::PutDataPath path);

  [[nodiscard]] UploadPartResult UploadPartGds(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      const std::string& rdma_token);

  [[nodiscard]] UploadPartResult UploadPartUcx(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t remote_addr, const std::string& packed_rkey,
      const std::string& client_ucx_addr);

  [[nodiscard]] CompleteResult CompleteUpload(
      const std::string& upload_id,
      const std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>& client_parts);

  [[nodiscard]] ProxyStatus AbortUpload(const std::string& upload_id);

 private:
  // part 校验 / final etag 计算（业务规则归服务层）。
  [[nodiscard]] ProxyStatus ValidateParts(const std::vector<PartRecord>& parts);
  [[nodiscard]] std::string ComputeFinalETag(const std::vector<PartRecord>& parts);

  IUploadIndex* index_;
  BlockStorage* block_storage_;
};

}  // namespace us3_turbo::proxy
