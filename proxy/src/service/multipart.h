#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"
#include "proxy/src/index/upload_index.h"

namespace us3_turbo::proxy {

class UfileAcClient;  // 前向声明，定义见 storage/ufile_ac_client.h

// 分段上传输出：成功时由服务层填充，接口层据此回填 response。
struct UploadPartOutput {
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

struct CompleteOutput {
  std::string   object_id;
  std::string   etag;
  std::uint64_t object_size{0};
};

// 分段上传服务：编排 Create/UploadPart/Complete/Abort，业务规则（part 校验 /
// final etag / client etag 比对）在本类。GDS/UCX 各自独立方法。依赖
// IUploadIndex*（mock/Mongo 无差别替换）+ UfileAcClient*（block 切分转发）。
// 成功返回 0，失败先 spdlog 再 return 错误码。AbortUpload 幂等保持 bool。
class Multipart {
 public:
  // 直接持有 UfileAcClient*（main 装配注入，本类不拥有）；替代旧 BlockStorage* 透传层。
  Multipart(IUploadIndex* index, UfileAcClient* client);

  [[nodiscard]] int CreateUpload(
      const std::string& request_id,
      const std::string& bucket, const std::string& key,
      PutDataPath path,
      std::string& out_upload_id);

  [[nodiscard]] int UploadPartGds(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      const std::string& rdma_token,
      UploadPartOutput& out);

  [[nodiscard]] int UploadPartUcx(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t remote_addr, const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      UploadPartOutput& out);

  [[nodiscard]] int CompleteUpload(
      const std::string& request_id,
      const std::string& upload_id,
      const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts,
      CompleteOutput& out);

  [[nodiscard]] bool AbortUpload(const std::string& request_id,
                                 const std::string& upload_id);  // 幂等，恒 true

 private:
  // part 校验：失败先 LOG_WARN，返回非 0 错误码；成功返回 0。
  [[nodiscard]] int ValidateParts(const std::string& request_id,
                                   const std::vector<PartRecord>& parts);
  /* 16MB 对齐校验：除最后一个 part 外必须 == 16MB */
  [[nodiscard]] int ValidatePartSizes(const std::string& request_id,
                                       const std::vector<PartRecord>& parts);
  std::string ComputeFinalETag(const std::vector<PartRecord>& parts);

  IUploadIndex*  index_;
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
