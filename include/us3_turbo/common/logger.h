#pragma once

// logger.h — 应用日志（App 日志）：控制台 / 控制台+滚动文件双 sink，
// 按 [函数名:行号] / [函数名:行号][req=request_id] 前缀全链路追踪；
// LOG_* 宏自动填 __func__ 与 __LINE__，调用方不再手写函数名前缀。
//
// client / proxy 共用：client 默认仅控制台 sink（库被多进程嵌入，不写文件
// 以免与 proxy 争抢 logs/）；proxy 用控制台 + 文件双 sink（前缀如 "proxy"）。
//
// 两类宏：
// - LOG_INFO/WARN/ERROR/DEBUG(rid, fmt, ...)  请求日志，输出
// [func:line][req=rid] 消息
// - LOG_SYS_INFO/WARN/ERROR/DEBUG(fmt, ...) 进程日志（启动/停止、构造期等无请求
//   上下文），输出 [func:line] 消息（无 [req=] 前缀）

#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>
#include <sys/stat.h>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::common {

namespace detail {

/* 生成带时间戳的日志文件名 logs/<prefix>-YYYY-MM-DD-HH-MM.log */
inline std::string MakeLogFileName(std::string_view prefix) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H-%M", &tm);
  std::string name = "logs/";
  name.append(prefix.data(), prefix.size());
  name += '-';
  name += buf;
  name += ".log";
  return name;
}

/* 创建 logs/ 目录（已存在则跳过）。 */
inline void EnsureLogDir() {
  struct stat st {};
  if (::stat("logs", &st) == 0) return;  // already exists
  (void)::mkdir("logs", 0755);
}

}  // namespace detail

/* 应用日志：控制台 / 控制台+滚动文件。通过 LOG_INFO/LOG_SYS_INFO 等宏自动填充
 * 函数名与行号，分别用于请求日志和进程日志。 */
class Logger {
 public:
  /* 仅控制台 sink（client 默认）。level 可配。 */
  static void Init(spdlog::level::level_enum level = spdlog::level::info) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("us3turbo", console_sink);
    logger->set_level(level);
    // 格式：[时间][级别] 内容，%v 由 LOG_* 宏拼好（已含
    // [func:line][req=rid]）。
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$] %v");
    spdlog::set_default_logger(logger);
  }

  /* 控制台 + 滚动文件 sink（proxy 用）。file_prefix 决定文件名前缀，如
   * "proxy"。 */
  static void Init(spdlog::level::level_enum level,
                   std::string_view file_prefix,
                   std::size_t max_file_size_mb = 50,
                   std::size_t max_files = 10) {
    detail::EnsureLogDir();
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        detail::MakeLogFileName(file_prefix), max_file_size_mb * 1024 * 1024,
        max_files);
    spdlog::sinks_init_list sinks{console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("us3turbo", sinks);
    logger->set_level(level);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$] %v");
    spdlog::set_default_logger(logger);
  }

  /* 请求日志（带 request_id）：[func:line][req=rid] 消息 */

  template <typename... Args>
  static void Info(std::string_view func, int line, std::string_view rid,
                   fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info("[{}:{}][req={}] {}", func, line, rid,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void Warn(std::string_view func, int line, std::string_view rid,
                   fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::warn("[{}:{}][req={}] {}", func, line, rid,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void Error(std::string_view func, int line, std::string_view rid,
                    fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::error("[{}:{}][req={}] {}", func, line, rid,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void Debug(std::string_view func, int line, std::string_view rid,
                    fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::debug("[{}:{}][req={}] {}", func, line, rid,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }

  /* 进程日志（无 request_id）：[func:line] 消息，用于启动/停止、构造期等无请求
   * 上下文场景 */

  template <typename... Args>
  static void SysInfo(std::string_view func, int line,
                      fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info("[{}:{}] {}", func, line,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void SysWarn(std::string_view func, int line,
                      fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::warn("[{}:{}] {}", func, line,
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void SysError(std::string_view func, int line,
                       fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::error("[{}:{}] {}", func, line,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void SysDebug(std::string_view func, int line,
                       fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::debug("[{}:{}] {}", func, line,
                  fmt::format(fmt, std::forward<Args>(args)...));
  }
};

/* 请求日志宏（带 request_id） */
#define LOG_INFO(rid, fmt, ...) \
  us3_turbo::common::Logger::Info(__func__, __LINE__, rid, fmt, ##__VA_ARGS__)
#define LOG_WARN(rid, fmt, ...) \
  us3_turbo::common::Logger::Warn(__func__, __LINE__, rid, fmt, ##__VA_ARGS__)
#define LOG_ERROR(rid, fmt, ...) \
  us3_turbo::common::Logger::Error(__func__, __LINE__, rid, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(rid, fmt, ...) \
  us3_turbo::common::Logger::Debug(__func__, __LINE__, rid, fmt, ##__VA_ARGS__)

/* 进程日志宏（无 request_id） */
#define LOG_SYS_INFO(fmt, ...) \
  us3_turbo::common::Logger::SysInfo(__func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_SYS_WARN(fmt, ...) \
  us3_turbo::common::Logger::SysWarn(__func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_SYS_ERROR(fmt, ...) \
  us3_turbo::common::Logger::SysError(__func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_SYS_DEBUG(fmt, ...) \
  us3_turbo::common::Logger::SysDebug(__func__, __LINE__, fmt, ##__VA_ARGS__)

}  // namespace us3_turbo::common
