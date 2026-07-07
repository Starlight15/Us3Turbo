#pragma once

// logger.h — 应用日志（App 日志）：rotating 文件 + 控制台双 sink，
// 按 [函数名][req=request_id] 全链路追踪；LOG_* 宏自动填 __func__。
//
// 与 access_logger 分离：App 日志人类可读、按大小滚动、受级别控制；
// Access 日志结构化、按天切分、永久开启（见 access_logger.h）。

#include <cstddef>
#include <string_view>

#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

/**
 * @brief 应用日志：proxy-YYYY-MM-DD-HH-MM.log，按大小滚动，支持 info/debug 级别。
 *
 * 输出格式：
 *   [时间][级别] [函数名][req=request_id] 消息内容
 *
 * 文件策略：
 *   - 按大小滚动（可配置，默认 50MB）
 *   - 保留文件数可配置（默认 10 个）
 *   - 文件名带启动时间戳：proxy-2026-07-07-10-20.log
 *
 * 使用：通过宏自动填充函数名
 *   LOG_INFO(request_id, "msg {}", arg);
 *   LOG_WARN(request_id, "error code={}", code);
 *   LOG_DEBUG(request_id, "detail info");
 */
class Logger {
 public:
  // 初始化（main 函数调用一次）。
  static void Init(spdlog::level::level_enum level = spdlog::level::info,
                   std::size_t max_file_size_mb = 50,
                   std::size_t max_files = 10);

  // Info 级别（关键步骤）。
  template <typename... Args>
  static void Info(std::string_view func, std::string_view request_id,
                   fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info("[{}][req={}] {}", func, request_id,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  // Warn 级别（参数错误、业务异常）。
  template <typename... Args>
  static void Warn(std::string_view func, std::string_view request_id,
                   fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::warn("[{}][req={}] {}", func, request_id,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  // Error 级别（系统错误、RPC 失败）。
  template <typename... Args>
  static void Error(std::string_view func, std::string_view request_id,
                    fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::error("[{}][req={}] {}", func, request_id,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }

  // Debug 级别（详细调试信息）。
  template <typename... Args>
  static void Debug(std::string_view func, std::string_view request_id,
                    fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::debug("[{}][req={}] {}", func, request_id,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }
};

// 宏简化调用（自动填充函数名）。
#define LOG_INFO(rid, fmt, ...)  \
  us3_turbo::proxy::Logger::Info(__func__, rid, fmt, ##__VA_ARGS__)
#define LOG_WARN(rid, fmt, ...)  \
  us3_turbo::proxy::Logger::Warn(__func__, rid, fmt, ##__VA_ARGS__)
#define LOG_ERROR(rid, fmt, ...) \
  us3_turbo::proxy::Logger::Error(__func__, rid, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(rid, fmt, ...) \
  us3_turbo::proxy::Logger::Debug(__func__, rid, fmt, ##__VA_ARGS__)

}  // namespace us3_turbo::proxy
