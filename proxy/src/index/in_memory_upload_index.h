#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "control_plane.pb.h"
#include "proxy/src/index/upload_index.h"

namespace us3_turbo::proxy {

// 纯内存 mock 实现 IUploadIndex。沿用旧 SessionManager 的并发模型：
// sessions_ 用 shared_mutex（读多写少），单 session 的 parts 用 parts_mu。
// 只做元数据 CRUD，无校验 / etag / client 比对（业务规则在服务层）。
class InMemoryUploadIndex final : public IUploadIndex {
 public:
  InMemoryUploadIndex() = default;
  ~InMemoryUploadIndex() override = default;

  InMemoryUploadIndex(const InMemoryUploadIndex&)            = delete;
  InMemoryUploadIndex& operator=(const InMemoryUploadIndex&) = delete;

  [[nodiscard]] std::string Create(
      const std::string& bucket, const std::string& key,
      PutDataPath path) override;

  [[nodiscard]] bool Get(
      const std::string& upload_id, UploadRecord& out) override;

  [[nodiscard]] bool AddPart(
      const std::string& upload_id, const PartRecord& part) override;

  [[nodiscard]] bool ListParts(
      const std::string& upload_id, std::vector<PartRecord>& out) override;

  void Remove(const std::string& upload_id) override;
  void RemoveExpired(std::int64_t ttl_ms) override;

  // ✅ 新增接口（增量写索引）
  [[nodiscard]] bool UpdateMergedSize(
      const std::string& upload_id, std::uint64_t merged_size) override;
  [[nodiscard]] bool UpdateLastMergedPart(
      const std::string& upload_id, std::int32_t part_number) override;

 private:
  struct Entry {
    UploadRecord            record;
    std::vector<PartRecord> parts;
    mutable std::mutex      parts_mu;
  };

  std::unordered_map<std::string, std::unique_ptr<Entry>> sessions_;
  std::shared_mutex                                        sessions_mu_;
};

}  // namespace us3_turbo::proxy
