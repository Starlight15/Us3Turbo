#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "proxy/src/api/control_plane_api.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/index/in_memory_upload_index.h"
#include "proxy/src/logging/access_logger.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/service/multipart.h"
#include "proxy/src/service/single_put.h"
#include "proxy/src/storage/backend_gateway.h"
#include "proxy/src/storage/block_storage.h"

namespace {

// 解析 --log_level 字符串为 spdlog 级别（未知值回落 info）。
spdlog::level::level_enum ParseLogLevel(const std::string& s) {
  if (s == "debug") return spdlog::level::debug;
  if (s == "info")  return spdlog::level::info;
  if (s == "warn")  return spdlog::level::warn;
  if (s == "error") return spdlog::level::err;
  return spdlog::level::info;
}

void RunUntilAskedToQuit() {
  sigset_t mask{};
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);
  spdlog::info("proxy running, press Ctrl+C to quit");
  int signo = 0;
  sigwait(&mask, &signo);
  spdlog::info("received signal {}, shutting down", signo);
}

}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // 初始化 App 日志（rotating 文件 + 控制台双 sink）。
  us3_turbo::proxy::Logger::Init(
      ParseLogLevel(FLAGS_log_level),
      static_cast<std::size_t>(FLAGS_log_max_size_mb),
      static_cast<std::size_t>(FLAGS_log_max_files));

  // 初始化 Access 审计日志（单例，按天切分，永久开启）。
  us3_turbo::proxy::AccessLogger::Instance();

  spdlog::info("proxy starting on {}:{}", FLAGS_bind_host, FLAGS_proxy_port);

  // 依赖注入装配：自底向上，存储层最长命（栈底），接口层最上，析构逆序安全。
  auto gateway       = std::make_unique<us3_turbo::proxy::BackendGateway>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms);
  auto block_storage = std::make_unique<us3_turbo::proxy::BlockStorage>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms,
      FLAGS_backend_block_size_bytes);
  auto index         = std::make_unique<us3_turbo::proxy::InMemoryUploadIndex>();
  auto single_put    = std::make_unique<us3_turbo::proxy::SinglePut>(
      gateway.get());
  auto multipart     = std::make_unique<us3_turbo::proxy::Multipart>(
      index.get(), block_storage.get());

  us3_turbo::proxy::ControlPlaneApi service(
      std::move(single_put), std::move(multipart), index.get());

  brpc::Server server;
  if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    spdlog::error("failed to register control-plane service");
    return EXIT_FAILURE;
  }

  brpc::ServerOptions options;
  options.num_threads = FLAGS_num_threads;

  const std::string endpoint =
      FLAGS_bind_host + ":" + std::to_string(FLAGS_proxy_port);
  if (server.Start(endpoint.c_str(), &options) != 0) {
    spdlog::error("failed to start brpc server on {}", endpoint);
    return EXIT_FAILURE;
  }

  spdlog::info("proxy control-plane listening on {}", endpoint);
  RunUntilAskedToQuit();

  server.Stop(0);
  server.Join();
  spdlog::info("proxy stopped");
  return EXIT_SUCCESS;
}
