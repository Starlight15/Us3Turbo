#pragma once

// session_manager.h — proxy 分段上传会话管理器（纯内存）。
//
// 职责：CreateSession 生成 upload_id；GetSessionPath 锁内读 path；AddPart
// 追加 part 元数据（同 part_number 覆盖）；CompleteSession 校验 part 列表
// 升序无重复 + 生成最终 object_id/etag；CleanupSession /
// CleanupExpiredSessions 清理。
//
// 线程安全：sessions_ 用 shared_mutex（读多写少）；单 session 的 parts 用
// UploadSession::parts_mu。CompleteSession 在持 sessions_mu_ 读锁 + parts_mu
// 后串行完成，自身不做 CleanupSession（由调用方成功后单独调）。

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "proxy/src/multipart/upload_session.h"

namespace us3_turbo::proxy {

class SessionManager {
 public:
  SessionManager();
  ~SessionManager();

  SessionManager(const SessionManager&)            = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  /** @brief 创建新会话，返回 upload_id（UUID）。 */
  std::string CreateSession(const std::string& bucket,
                            const std::string& key,
                            ::us3_turbo::proxy::PutDataPath path);

  /**
   * @brief 锁内读取会话 path；会话不存在返回 false。
   *
   * 替代持裸指针读 session：调用方无需锁外引用，CompleteSession/Abort 可安全并发。
   */
  bool GetSessionPath(const std::string& upload_id,
                      ::us3_turbo::proxy::PutDataPath& out_path);

  /** @brief 追加 part 元数据（同 part_number 覆盖）。会话不存在返回 false。 */
  bool AddPart(const std::string& upload_id, const PartMetadata& part);

  /**
   * @brief 完成会话：按 part_number 排序、校验升序无重复、（可选）校验 client
   *        提供的 part etag，生成最终 object_id/etag/size。
   * @return 校验通过返回 true 并填充 out_*；失败返回 false 并设 out_error。
   */
  bool CompleteSession(
      const std::string& upload_id,
      const std::vector<::us3_turbo::proxy::CompleteMultipartUploadRequest_PartInfo>& client_parts,
      std::string& out_object_id,
      std::string& out_etag,
      std::uint64_t& out_size,
      std::string& out_error);

  /** @brief 立即删除会话（幂等，不存在也 ok）。 */
  void CleanupSession(const std::string& upload_id);

  /** @brief 删除超过 ttl_ms 的会话（后台清理线程调用）。 */
  void CleanupExpiredSessions(std::int64_t ttl_ms);

 private:
  // 在持 parts_mu 的前提下校验 part 列表升序无重复（s3 语义：允许间隙如 1,3,5）。
  bool ValidatePartList(const UploadSession& session, std::string& error);

  // 计算 final etag：单 part → 该 part 的 etag；多 part → SHA1(各 etag 拼接)
  // 前缀 4 字节 little-endian part count 再 base64。
  std::string ComputeFinalETag(const std::vector<PartMetadata>& parts);

  std::unordered_map<std::string, std::unique_ptr<UploadSession>> sessions_;
  std::shared_mutex sessions_mu_;
};

}  // namespace us3_turbo::proxy
