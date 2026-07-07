#pragma once

// access_logger.h — Access 审计日志：每个请求一条结构化记录，永久开启，
// 不受 log_level 影响。按天切分、保留 30 天，便于 awk/grep 统计。

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

/**
 * @brief Access 日志：记录每个请求的审计信息（永久开启，不受 log_level 影响）。
 *
 * 输出格式（固定，%v 内容）：
 *   method|request_id|bucket|key|status_code|bytes|latency_ms
 * 完整一行（pattern 前缀时间戳）：
 *   YYYY-MM-DD HH:MM:SS|method|request_id|bucket|key|status_code|bytes|latency_ms
 *
 * multipart 的 UploadPart/Complete/Abort 请求 proto 无 bucket/key 字段，
 * 调用方传 "-" 占位；其 upload_id 由 App 日志（LOG_*）携带，不进 Access 列。
 *
 * 文件策略：
 *   - 按天切分（每天 00:00），文件名：access-YYYY-MM-DD.log
 *   - 保留 30 天
 */
class AccessLogger {
 public:
  static AccessLogger& Instance();

  // 记录一次请求（接口层在 handler 结束时调用）。
  // status_code: 0=成功，非 0=PROXY_ERR_*。
  // bucket/key 无对应字段时传 "-"（multipart part/complete/abort）。
  void LogRequest(
      std::string_view method,           // "GdsPut" / "UploadPartGds" / ...
      std::string_view request_id,
      std::string_view bucket,
      std::string_view key,
      int status_code,
      std::uint64_t bytes,
      std::chrono::milliseconds latency);

 private:
  AccessLogger();
  ~AccessLogger() = default;
  AccessLogger(const AccessLogger&) = delete;
  AccessLogger& operator=(const AccessLogger&) = delete;

  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo::proxy
