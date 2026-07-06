#pragma once

// upload_session.h — proxy 内存态分段上传会话。
//
// 一个 UploadSession 对应一次 multipart upload：CreateMultipartUpload 时建，
// CompleteMultipartUpload / TTL 过期时删。part 列表（PartMetadata）按到达顺序
// 追加，CompleteSession 时按 part_number 排序后做连续性校验。
//
// 对齐 s3proxy 的 Us3MinitIdxInfo / Us3PartElement 语义（v1 仅保留 Us3Turbo
// 实际用到的字段：upload_id/bucket/key/path/parts/created_at），其余 s3proxy
// 字段（obj_id/file_id/set_id/blk_*）暂留占位、不参与 v1 逻辑。

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "control_plane.pb.h"

namespace us3_turbo::proxy {

/** @brief 单个 part 的元数据（对齐 s3proxy Us3PartElement）。 */
struct PartMetadata {
  std::uint32_t part_number{0};   // 1-based
  std::uint64_t part_size{0};
  std::string   etag;             // part 级 ETag（proxy 汇总 block etag 得到）
  std::int64_t  upload_time_ms{0};
};

/** @brief 一个分段上传会话（对齐 s3proxy Us3MinitIdxInfo）。 */
struct UploadSession {
  std::string upload_id;
  std::string bucket;
  std::string key;
  ::us3_turbo::proxy::PutDataPath path{::us3_turbo::proxy::PATH_NONE};

  std::int64_t created_at_ms{0};
  int          status{0};          // 0=未完成, 1=已完成

  std::vector<PartMetadata> parts;
  mutable std::mutex        parts_mu;  // 保护 parts 并发写

  [[nodiscard]] bool IsExpired(std::int64_t now_ms, std::int64_t ttl_ms) const {
    return (now_ms - created_at_ms) > ttl_ms;
  }

  [[nodiscard]] std::uint64_t TotalSize() const {
    std::uint64_t sum = 0;
    for (const auto& p : parts) sum += p.part_size;
    return sum;
  }
};

}  // namespace us3_turbo::proxy
