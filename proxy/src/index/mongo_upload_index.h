#pragma once

#include <memory>
#include <string>

#include "proxy/src/index/dbgate_client.h"
#include "proxy/src/index/upload_index.h"

namespace us3_turbo::proxy {

/* MongoDB 持久化索引实现
 * 通过 DBGateClient 操作三张表：minit_col(multipart
 * 会话)、part_col(分段记录)、fileidx_col(对象元数据) */
class MongoUploadIndex final : public IUploadIndex {
 public:
  explicit MongoUploadIndex(DBGateClient* client) : client_(client) {}

  // ============================ 分段上传（minit_col + part_col）
  // ============================

  /* 创建上传会话，写入 minit_col，返回 upload_id */
  [[nodiscard]] std::string Create(const std::string& bucket,
                                   const std::string& key,
                                   PutDataPath path) override;

  /* 按 upload_id 查询上传记录，未找到返回 false */
  [[nodiscard]] bool Get(const std::string& upload_id,
                         UploadRecord& out) override;

  /* 追加分段记录到 part_col */
  [[nodiscard]] bool AddPart(const std::string& upload_id,
                             const PartRecord& part) override;

  /* 列举指定上传会话的所有分段 */
  [[nodiscard]] bool ListParts(const std::string& upload_id,
                               std::vector<PartRecord>& out) override;

  /* 删除上传会话及其分段记录 */
  void Remove(const std::string& upload_id) override;
  /* 清理超过 ttl_ms 的过期上传会话 */
  void RemoveExpired(std::int64_t ttl_ms) override;

  /* 更新上传会话的已合并大小 */
  [[nodiscard]] bool UpdateMergedSize(const std::string& upload_id,
                                      std::uint64_t merged_size) override;
  /* 更新上传会话的最后合并分段号 */
  [[nodiscard]] bool UpdateLastMergedPart(const std::string& upload_id,
                                          std::int32_t part_number) override;

  // ============================ 单步上传 + GET（fileidx_col）
  // ============================

  /* 插入对象元数据到 fileidx_col，single_put 和 Complete 均调用 */
  [[nodiscard]] bool InsertFileIdx(const std::string& bucket,
                                   const std::string& key,
                                   const std::string& first_object,
                                   std::uint64_t block_size,
                                   std::uint64_t filesize,
                                   const std::string& hash) override;

  /* 按 bucket+key 查询对象元数据，GetObject 第一步 */
  [[nodiscard]] bool GetFileIdx(const std::string& bucket,
                                const std::string& key,
                                FileIdxRecord& out) override;

 private:
  DBGateClient* client_;  // 非拥有指针，外部管理生命周期
};

}  // namespace us3_turbo::proxy
