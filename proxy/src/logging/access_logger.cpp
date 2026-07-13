#include "proxy/src/logging/access_logger.h"

#include <string_view>

namespace us3_turbo::proxy {

AccessLogger& AccessLogger::Instance() {
  static AccessLogger instance;
  return instance;
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
