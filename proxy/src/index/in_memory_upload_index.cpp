#include "proxy/src/index/in_memory_upload_index.h"

#include <algorithm>
#include <shared_mutex>
#include <utility>

#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

std::string InMemoryUploadIndex::Create(
    const std::string& bucket, const std::string& key,
    PutDataPath path) {
  auto entry = std::make_unique<Entry>();
  entry->record.upload_id     = utils::GenUuid();
  entry->record.bucket        = bucket;
  entry->record.key           = key;
  entry->record.path          = path;
  entry->record.created_at_ms = utils::NowMs();

  // ✅ 初始化新字段（对齐 s3proxy）
  entry->record.obj_id           = utils::GenUuid();  // 最终对象 ID
  entry->record.block_size       = 4ULL * 1024 * 1024;  // 4MB 固定
  entry->record.merged_size      = 0;
  entry->record.last_merged_part = 0;
  entry->record.status           = 0;  // 0=进行中

  const std::string upload_id = entry->record.upload_id;
  {
    std::unique_lock lock(sessions_mu_);
    sessions_[upload_id] = std::move(entry);
  }
  return upload_id;
}

bool InMemoryUploadIndex::Get(const std::string& upload_id,
                              UploadRecord& out) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;
  // parts_mu 兼作 record 写入锁（见 UpdateMergedSize 等），拷贝时加锁防撕裂读。
  std::lock_guard plk(it->second->parts_mu);
  out = it->second->record;
  return true;
}

bool InMemoryUploadIndex::AddPart(const std::string& upload_id,
                                  const PartRecord& part) {
  // 持读锁取 entry（与 Remove 的写锁互斥），再用 parts_mu 改 parts。
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;
  Entry* entry = it->second.get();

  std::lock_guard plk(entry->parts_mu);
  auto p = std::find_if(entry->parts.begin(), entry->parts.end(),
                        [&](const PartRecord& q) {
                          return q.part_number == part.part_number;
                        });
  if (p != entry->parts.end()) {
    if (p->part_size != part.part_size) {
      LOG_SYS_WARN("part {} size changed {} -> {} (overwrite), upload={}",
                   part.part_number, p->part_size, part.part_size, upload_id);
    }
    *p = part;  // 同 part_number 覆盖
  } else {
    entry->parts.push_back(part);
  }
  return true;
}

bool InMemoryUploadIndex::ListParts(const std::string& upload_id,
                                    std::vector<PartRecord>& out) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;
  Entry* entry = it->second.get();

  std::lock_guard plk(entry->parts_mu);
  out = entry->parts;  // 未排序副本，排序/校验由服务层做
  return true;
}

void InMemoryUploadIndex::Remove(const std::string& upload_id) {
  std::unique_lock lock(sessions_mu_);
  sessions_.erase(upload_id);  // 幂等，不存在也 ok
}

void InMemoryUploadIndex::RemoveExpired(std::int64_t ttl_ms) {
  const std::int64_t now = utils::NowMs();
  std::unique_lock lock(sessions_mu_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if ((now - it->second->record.created_at_ms) > ttl_ms) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

bool InMemoryUploadIndex::UpdateMergedSize(const std::string& upload_id,
                                           std::uint64_t merged_size) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;

  std::lock_guard plk(it->second->parts_mu);
  it->second->record.merged_size = merged_size;
  return true;
}

bool InMemoryUploadIndex::UpdateLastMergedPart(const std::string& upload_id,
                                               std::int32_t part_number) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;

  std::lock_guard plk(it->second->parts_mu);
  it->second->record.last_merged_part = part_number;
  return true;
}

}  // namespace us3_turbo::proxy
