#include "proxy/src/logging/access_logger.h"

#include <string_view>

#include <spdlog/sinks/daily_file_sink.h>

namespace us3_turbo::proxy {

namespace {

// Access 日志固定文件名模板：logs/access-YYYY-MM-DD.log
constexpr const char* kAccessLogPattern = "logs/access-%Y-%m-%d.log";

}  // namespace

AccessLogger& AccessLogger::Instance() {
  static AccessLogger instance;
  return instance;
}

AccessLogger::AccessLogger() {
  // 独立 logger：按天切分
  auto sink = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(
      kAccessLogPattern,
      0,     // rotation hour
      0,     // rotation minute
      false, // 不截断已有文件
      30);   // 保留 30 天

  logger_ = std::make_shared<spdlog::logger>("access", sink);
  logger_->set_level(spdlog::level::info);

  // 内容为 method|rid|bucket|key|status|bytes|latency。
  logger_->set_pattern("%Y-%m-%d %H:%M:%S|%v");

  spdlog::register_logger(logger_);
}

void AccessLogger::LogRequest(
    std::string_view method,
    std::string_view request_id,
    std::string_view bucket,
    std::string_view key,
    int status_code,
    std::uint64_t bytes,
    std::chrono::milliseconds latency) {
  // 格式：method|request_id|bucket|key|status|bytes|latency_ms
  logger_->info("{}|{}|{}|{}|{}|{}|{}",
                method, request_id, bucket, key,
                status_code, bytes, latency.count());
}

}  // namespace us3_turbo::proxy
