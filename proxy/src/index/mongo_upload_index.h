#pragma once

#include <memory>
#include <string>

#include "proxy/src/index/upload_index.h"
#include "proxy/src/index/dbgate_client.h"

namespace us3_turbo::proxy {

/**
 * @brief MongoDB 持久化索引实现
 *
 * 通过 DBGateClient 操作三张表：
 * - minit_col: multipart 会话
 * - part_col: 分段记录
 * - fileidx_col: 对象元数据
 */
class MongoUploadIndex final : public IUploadIndex {
 public:
  explicit MongoUploadIndex(DBGateClient* client) : client_(client) {}

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

  [[nodiscard]] bool UpdateMergedSize(
      const std::string& upload_id, std::uint64_t merged_size) override;
  [[nodiscard]] bool UpdateLastMergedPart(
      const std::string& upload_id, std::int32_t part_number) override;

  [[nodiscard]] bool InsertFileIdx(
      const std::string& bucket,
      const std::string& key,
      const std::string& first_object,
      std::uint64_t block_size,
      std::uint64_t filesize,
      const std::string& hash) override;

  [[nodiscard]] bool GetFileIdx(
      const std::string& bucket,
      const std::string& key,
      FileIdxRecord& out) override;

 private:
  DBGateClient* client_;  // 不拥有所有权，由外部管理生命周期
};

}  // namespace us3_turbo::proxy
