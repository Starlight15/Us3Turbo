#pragma once

// logger.h — 应用日志（App 日志）：rotating 文件 + 控制台双 sink，
// 按 [函数名] / [函数名][req=request_id] 前缀全链路追踪；LOG_* 宏自动填
// __func__。
//
// 与 access_logger 分离：App 日志人类可读、按大小滚动、受级别控制；
// Access 日志结构化、按天切分、永久开启（见 access_logger.h）。
//
// 两类宏：
// - LOG_INFO/WARN/ERROR/DEBUG(rid, fmt, ...)  请求日志，输出 [func][req=rid]
// 消息
// - LOG_SYS_INFO/WARN/ERROR/DEBUG(fmt, ...)   进程日志（main
// 启动/停止、构造期等
//   无请求上下文），输出 [func] 消息（无 [req=] 前缀）

#include <cstddef>
#include <string_view>

#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

/* 应用日志：proxy-YYYY-MM-DD-HH-MM.log，按大小滚动，支持 info/debug 级别。
 * 按大小滚动（默认 50MB），保留文件数可配置（默认 10 个），文件名带启动时间戳。
 * 通过 LOG_INFO/LOG_SYS_INFO 等宏自动填充函数名，分别用于请求日志和进程日志。
 */
class Logger {
 public:
  /* Initialize the rotating file + console dual-sink logger. */
  static void Init(spdlog::level::level_enum level = spdlog::level::info,
                   std::size_t max_file_size_mb = 50, std::size_t max_files = 10);

  /* 请求日志（带 request_id）：[函数名][req=rid] 消息 */

  template <typename... Args>
  static void Info(std::string_view func, std::string_view request_id,
                   fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info("[{}][req={}] {}", func, request_id,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void Warn(std::string_view func, std::string_view request_id,
                   fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::warn("[{}][req={}] {}", func, request_id,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void Error(std::string_view func, std::string_view request_id,
                    fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::error("[{}][req={}] {}", func, request_id,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void Debug(std::string_view func, std::string_view request_id,
                    fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::debug("[{}][req={}] {}", func, request_id,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }

  /* 进程日志（无 request_id）：[函数名]
   * 消息，用于启动/停止、构造期等无请求上下文场景 */

  template <typename... Args>
  static void SysInfo(std::string_view func, fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info("[{}] {}", func, fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void SysWarn(std::string_view func, fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::warn("[{}] {}", func, fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void SysError(std::string_view func, fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::error("[{}] {}", func, fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void SysDebug(std::string_view func, fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::debug("[{}] {}", func, fmt::format(fmt, std::forward<Args>(args)...));
  }
};

/* 请求日志宏（带 request_id） */
#define LOG_INFO(rid, fmt, ...) us3_turbo::proxy::Logger::Info(__func__, rid, fmt, ##__VA_ARGS__)
#define LOG_WARN(rid, fmt, ...) us3_turbo::proxy::Logger::Warn(__func__, rid, fmt, ##__VA_ARGS__)
#define LOG_ERROR(rid, fmt, ...) us3_turbo::proxy::Logger::Error(__func__, rid, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(rid, fmt, ...) us3_turbo::proxy::Logger::Debug(__func__, rid, fmt, ##__VA_ARGS__)

/* 进程日志宏（无 request_id） */
#define LOG_SYS_INFO(fmt, ...) us3_turbo::proxy::Logger::SysInfo(__func__, fmt, ##__VA_ARGS__)
#define LOG_SYS_WARN(fmt, ...) us3_turbo::proxy::Logger::SysWarn(__func__, fmt, ##__VA_ARGS__)
#define LOG_SYS_ERROR(fmt, ...) us3_turbo::proxy::Logger::SysError(__func__, fmt, ##__VA_ARGS__)
#define LOG_SYS_DEBUG(fmt, ...) us3_turbo::proxy::Logger::SysDebug(__func__, fmt, ##__VA_ARGS__)

}  // namespace us3_turbo::proxy
