#include "proxy/src/multipart/session_manager.h"

#include <algorithm>
#include <shared_mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "proxy/src/common/utils.h"

namespace us3_turbo::proxy {

SessionManager::SessionManager() = default;
SessionManager::~SessionManager() = default;

std::string SessionManager::CreateSession(const std::string& bucket,
                                          const std::string& key,
                                          ::us3_turbo::proxy::PutDataPath path) {
  auto session = std::make_unique<UploadSession>();
  session->upload_id    = utils::GenUuid();
  session->bucket       = bucket;
  session->key          = key;
  session->path         = path;
  session->created_at_ms = utils::NowMs();
  session->status       = 0;

  const std::string upload_id = session->upload_id;
  {
    std::unique_lock lock(sessions_mu_);
    sessions_[upload_id] = std::move(session);
  }
  return upload_id;
}

bool SessionManager::GetSessionPath(const std::string& upload_id,
                                    ::us3_turbo::proxy::PutDataPath& out_path) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;
  out_path = it->second->path;
  return true;
}

bool SessionManager::AddPart(const std::string& upload_id,
                             const PartMetadata& part) {
  // 持读锁取 session（与 CleanupSession 的写锁互斥），再用 parts_mu 改 parts。
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;
  UploadSession* session = it->second.get();

  std::lock_guard plk(session->parts_mu);
  auto p = std::find_if(session->parts.begin(), session->parts.end(),
                        [&](const PartMetadata& q) {
                          return q.part_number == part.part_number;
                        });
  if (p != session->parts.end()) {
    if (p->part_size != part.part_size) {
      spdlog::warn("AddPart: part {} size changed {} -> {} (overwrite)",
                   part.part_number, p->part_size, part.part_size);
    }
    *p = part;  // 同 part_number 覆盖
  } else {
    session->parts.push_back(part);
  }
  return true;
}

bool SessionManager::CompleteSession(
    const std::string& upload_id,
    const std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>& client_parts,
    std::string& out_object_id,
    std::string& out_etag,
    std::uint64_t& out_size,
    std::string& out_error) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) {
    out_error = "upload_id not found";
    return false;
  }
  UploadSession* session = it->second.get();

  std::lock_guard plk(session->parts_mu);

  // 1. 按 part_number 排序。
  std::sort(session->parts.begin(), session->parts.end(),
            [](const PartMetadata& a, const PartMetadata& b) {
              return a.part_number < b.part_number;
            });

  // 2. 升序无重复校验（s3 语义：允许间隙如 1,3,5）。
  if (!ValidatePartList(*session, out_error)) return false;

  // 3. 客户端提供 part 列表时校验 etag 匹配。
  if (!client_parts.empty()) {
    if (client_parts.size() != session->parts.size()) {
      out_error = "part count mismatch";
      return false;
    }
    for (std::size_t i = 0; i < client_parts.size(); ++i) {
      if (client_parts[i].part_number() != session->parts[i].part_number ||
          client_parts[i].etag() != session->parts[i].etag) {
        out_error = "part etag mismatch at part " +
                    std::to_string(session->parts[i].part_number);
        return false;
      }
    }
  }

  // 4. 生成最终 object_id / etag / size。
  out_object_id = session->bucket + "/" + session->key;
  out_etag = ComputeFinalETag(session->parts);
  out_size = session->TotalSize();
  session->status = 1;
  return true;
}

bool SessionManager::ValidatePartList(const UploadSession& session,
                                      std::string& error) {
  if (session.parts.empty()) {
    error = "no parts uploaded";
    return false;
  }
  // s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
  // parts 已在 CompleteSession 里按 part_number 排序，此处只校验严格升序。
  for (std::size_t i = 1; i < session.parts.size(); ++i) {
    if (session.parts[i].part_number <= session.parts[i - 1].part_number) {
      error = "part_number not strictly ascending at index " + std::to_string(i);
      return false;
    }
  }
  return true;
}

std::string SessionManager::ComputeFinalETag(
    const std::vector<PartMetadata>& parts) {
  // 对齐 s3proxy ETagByEtags：单 part 直接用其 etag，多 part 汇总（见 utils::CombineETags）。
  std::vector<std::string> etags;
  etags.reserve(parts.size());
  for (const auto& p : parts) etags.push_back(p.etag);
  return utils::CombineETags(etags);
}

void SessionManager::CleanupSession(const std::string& upload_id) {
  std::unique_lock lock(sessions_mu_);
  sessions_.erase(upload_id);
}

void SessionManager::CleanupExpiredSessions(std::int64_t ttl_ms) {
  const std::int64_t now = utils::NowMs();
  std::unique_lock lock(sessions_mu_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second->IsExpired(now, ttl_ms)) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace us3_turbo::proxy
