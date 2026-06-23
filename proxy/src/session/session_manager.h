#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace us3_turbo::proxy {

/**
 * @brief 生成并持有 proxy 控制面的 session 状态。
 *
 * 职责：
 * - 生成 session_id（格式 "pxs-<seq>"）和 ticket
 * - 存储 session 元信息（bucket/key/size）和完成状态
 * - 接受 backend ReportGdsPut 通知标记完成
 *
 * 线程安全：所有方法通过 std::mutex 保护 sessions_ map。
 *
 * 依赖：纯标准库，不依赖 brpc（可单元测试）。
 */
class SessionManager {
 public:
  struct Session {
    std::string session_id;  // "pxs-<seq>"
    std::string ticket;      // "tkt-<session_id>"
    std::string expire_at;   // ISO 8601，如 "2026-06-18T09:00:00Z"

    // PUT 元信息（OpenSession 填入）
    std::string bucket;
    std::string object_key;
    std::uint64_t expected_size{0};

    // 完成状态（ReportGdsPut 填入）
    enum class Status { kPending, kCompleted, kFailed };
    Status status{Status::kPending};
    std::string etag;
    std::uint32_t crc32c{0};
    std::uint64_t bytes_transferred{0};
  };

  // ttl_sec: session 有效期（秒），用于计算 expire_at。
  explicit SessionManager(std::int64_t ttl_sec);

  // 生成新 session，存入 map。session_id_hint 非空时直接用作 session_id。
  Session CreateSession(const std::string& session_id_hint,
                        const std::string& bucket,
                        const std::string& object_key,
                        std::uint64_t expected_size);

  // Backend ReportGdsPut 调用：标记 session 完成。
  // 返回 false 表示 session 不存在或已过期。
  bool CompleteSession(const std::string& session_id,
                       const std::string& etag,
                       std::uint32_t crc32c,
                       std::uint64_t bytes_transferred);

  // 查询 session（调试/监控用）。返回 nullptr 表示不存在。
  const Session* GetSession(const std::string& session_id);

  // 按 ticket 查询已完成的 session（CompleteUpload 用）。
  const Session* GetCompletedSession(const std::string& ticket);

 private:
  std::int64_t ttl_sec_;
  std::atomic<std::uint64_t> seq_{0};
  std::mutex mu_;
  std::unordered_map<std::string, Session> sessions_;
};

}  // namespace us3_turbo::proxy
