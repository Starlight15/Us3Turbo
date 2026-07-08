#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control_plane.pb.h"

namespace us3_turbo::proxy {

// part 元数据（对齐 s3proxy Us3PartElement，去掉业务方法）。
struct PartRecord {
  std::uint32_t part_number{0};   // 1-based
  std::uint64_t part_size{0};
  std::string   etag;             // part 级 ETag（block etag 汇总得到）
  std::int64_t  upload_time_ms{0};
};

// 会话元数据（对齐 s3proxy Us3MinitIdxInfo，去掉 TotalSize/IsExpired）。
struct UploadRecord {
  std::string  upload_id;
  std::string  bucket;
  std::string  key;
  PutDataPath path{PATH_NONE};
  std::int64_t created_at_ms{0};
};

// 纯被动元数据存储接口。内存 mock 与后续 MongoDB 实现同一接口，可无差别替换。
// 不含任何校验 / etag 计算 / client 比对（全在服务层）。
class IUploadIndex {
 public:
  virtual ~IUploadIndex() = default;

  /** @brief 创建新会话，返回 upload_id（UUID）。 */
  [[nodiscard]] virtual std::string Create(
      const std::string& bucket, const std::string& key,
      PutDataPath path) = 0;

  /** @brief 读会话（不存在返回 false）。纯读，不含业务判断。 */
  [[nodiscard]] virtual bool Get(
      const std::string& upload_id, UploadRecord& out) = 0;

  /** @brief 追加/覆盖 part（同 part_number 覆盖）。不存在返回 false。 */
  [[nodiscard]] virtual bool AddPart(
      const std::string& upload_id, const PartRecord& part) = 0;

  /** @brief 列出某会话所有 part（未排序，排序/校验由服务层做）。 */
  [[nodiscard]] virtual bool ListParts(
      const std::string& upload_id, std::vector<PartRecord>& out) = 0;

  /** @brief 删除会话（幂等）。 */
  virtual void Remove(const std::string& upload_id) = 0;

  /** @brief 删除超过 ttl_ms 的会话（后台清理线程调用）。 */
  virtual void RemoveExpired(std::int64_t ttl_ms) = 0;
};

}  // namespace us3_turbo::proxy
