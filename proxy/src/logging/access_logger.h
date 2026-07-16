#pragma once

// access_logger.h — Access 审计日志：每个请求一条结构化记录，永久开启，
// 不受 log_level 影响。按天切分、保留 30 天，便于 awk/grep 统计。

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

// Access 日志固定文件名模板：logs/access-YYYY-MM-DD.log
inline constexpr const char* kAccessLogPattern = "logs/access-%Y-%m-%d.log";

/* Access 审计日志：每个请求一条记录，永久开启不受 log_level 影响。
   按天切分(00:00)，文件名 access-YYYY-MM-DD.log，保留 30 天。 */
class AccessLogger {
 public:
  static AccessLogger& Instance();

  /* 记录一次请求，handler 结束时调用 */
  void LogRequest(std::string_view method, std::string_view request_id,
                  std::string_view bucket, std::string_view key,
                  int status_code, std::uint64_t bytes,
                  std::chrono::milliseconds latency);

 private:
  AccessLogger() {
    // 独立 logger：按天切分
    auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(
        kAccessLogPattern,
        0,      // rotation hour
        0,      // rotation minute
        false,  // 不截断已有文件
        30);    // 保留 30 天

    logger_ = std::make_shared<spdlog::logger>("access", sink);
    logger_->set_level(spdlog::level::info);

    // 内容为 method|rid|bucket|key|status|bytes|latency。
    logger_->set_pattern("%Y-%m-%d %H:%M:%S|%v");

    spdlog::register_logger(logger_);
  }
  ~AccessLogger() = default;
  AccessLogger(const AccessLogger&) = delete;
  AccessLogger& operator=(const AccessLogger&) = delete;

  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo::proxy
