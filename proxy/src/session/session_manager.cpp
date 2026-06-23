#include "proxy/src/session/session_manager.h"

#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace us3_turbo::proxy {

namespace {

// session_id 前缀，与 OpenSession 历史实现一致。
constexpr std::string_view kSessionIdPrefix = "pxs-";

}  // namespace

SessionManager::SessionManager(std::int64_t ttl_sec) : ttl_sec_(ttl_sec) {}

SessionManager::Session SessionManager::CreateSession(
    const std::string& session_id_hint,
    const std::string& bucket,
    const std::string& object_key,
    std::uint64_t expected_size) {
  // ---- 生成 session_id ----
  std::string session_id;
  if (!session_id_hint.empty()) {
    session_id = session_id_hint;
  } else {
    const auto seq = seq_.fetch_add(1, std::memory_order_relaxed);
    session_id = std::string(kSessionIdPrefix) + std::to_string(seq);
  }

  // ---- 生成 ticket ----
  const std::string ticket = std::string("tkt-") + session_id;

  // ---- expire_at ----
  const auto expire_time =
      std::chrono::system_clock::now() + std::chrono::seconds(ttl_sec_);
  const auto expire_at_time_t =
      std::chrono::system_clock::to_time_t(expire_time);
  // ISO 8601 近似格式，与 gateway SessionOpener 风格一致。
  // gmtime_r 替代 std::gmtime：后者返回进程级静态缓冲区，非线程安全，
  // 与并发调用冲突。
  char expire_buf[32] = {};
  struct tm tm_buf{};
  gmtime_r(&expire_at_time_t, &tm_buf);
  std::strftime(expire_buf, sizeof(expire_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

  Session session{
      .session_id = std::move(session_id),
      .ticket = std::move(ticket),
      .expire_at = std::string(expire_buf),
      .bucket = bucket,
      .object_key = object_key,
      .expected_size = expected_size,
      .status = Session::Status::kPending,
  };

  // 记入索引（key = session_id），供 ReportGdsPut / CompleteUpload 查询。
  {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_[session.session_id] = session;
  }
  return session;
}

bool SessionManager::CompleteSession(const std::string& session_id,
                                     const std::string& etag,
                                     std::uint32_t crc32c,
                                     std::uint64_t bytes_transferred) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return false;  // 不存在或已过期
  }
  it->second.status = Session::Status::kCompleted;
  it->second.etag = etag;
  it->second.crc32c = crc32c;
  it->second.bytes_transferred = bytes_transferred;
  return true;
}

const SessionManager::Session* SessionManager::GetSession(
    const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(session_id);
  return (it != sessions_.end()) ? &it->second : nullptr;
}

const SessionManager::Session* SessionManager::GetCompletedSession(
    const std::string& ticket) {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [_, session] : sessions_) {
    if (session.ticket == ticket && session.status == Session::Status::kCompleted) {
      return &session;
    }
  }
  return nullptr;
}

}  // namespace us3_turbo::proxy
