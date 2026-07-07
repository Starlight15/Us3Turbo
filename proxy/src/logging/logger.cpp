#include "proxy/src/logging/logger.h"

#include <ctime>
#include <string>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sys/stat.h>

namespace us3_turbo::proxy {

namespace {

// 生成带时间戳的日志文件名：logs/proxy-YYYY-MM-DD-HH-MM.log
std::string MakeLogFileName() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "logs/proxy-%Y-%m-%d-%H-%M.log", &tm);
  return buf;
}

// 创建 logs/ 目录
void EnsureLogDir() {
  struct stat st{};
  if (::stat("logs", &st) == 0) return;  // 已存在
  if (::mkdir("logs", 0755) == 0) return;
}

}  // namespace

void Logger::Init(spdlog::level::level_enum level,
                  std::size_t max_file_size_mb,
                  std::size_t max_files) {
  EnsureLogDir();

  // Sink 1: 控制台（带颜色，便于开发调试）。
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  // Sink 2: 文件（按大小滚动）。
  auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      MakeLogFileName(),
      max_file_size_mb * 1024 * 1024,  // MB → 字节
      max_files);

  // 双 sink 合并（同时输出到控制台和文件）。
  spdlog::sinks_init_list sinks{console_sink, file_sink};
  auto logger = std::make_shared<spdlog::logger>("proxy", sinks);
  logger->set_level(level);

  // 格式：[时间][级别] 内容（%v 已含 [函数名][req=...] 消息，由 LOG_* 宏拼好）。
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$] %v");

  // 设为全局默认：之后 spdlog::info/...（main.cpp 进程生命周期日志）与
  // LOG_* 宏（请求日志）均走此 logger，统一写 proxy-*.log + 控制台。
  spdlog::set_default_logger(logger);
}

}  // namespace us3_turbo::proxy
