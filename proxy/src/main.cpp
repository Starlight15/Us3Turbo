#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "proxy/src/api/proxy_service.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/index/in_memory_upload_index.h"
#include "proxy/src/logging/access_logger.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/service/multipart.h"
#include "proxy/src/service/single_put.h"
#include "proxy/src/storage/backend_gateway.h"
#include "proxy/src/storage/block_storage.h"

namespace {

// 初始化日志
void InitLogging() {
  spdlog::level::level_enum level = spdlog::level::info;
  if (FLAGS_log_level == "debug")      level = spdlog::level::debug;
  else if (FLAGS_log_level == "warn")  level = spdlog::level::warn;
  else if (FLAGS_log_level == "error") level = spdlog::level::err;
  us3_turbo::proxy::Logger::Init(
      level,
      static_cast<std::size_t>(FLAGS_log_max_size_mb),
      static_cast<std::size_t>(FLAGS_log_max_files));
  us3_turbo::proxy::AccessLogger::Instance();
}

// 依赖注入装配产物：存储层 + 索引层 + 接口层
// 成员析构逆序 = service→index→block_storage→gateway，保证 service 的 TTL 清理
struct AssembledStack {
  std::unique_ptr<us3_turbo::proxy::BackendGateway>      gateway;
  std::unique_ptr<us3_turbo::proxy::BlockStorage>        block_storage;
  std::unique_ptr<us3_turbo::proxy::InMemoryUploadIndex> index;
  std::unique_ptr<us3_turbo::proxy::ProxyService>     service;
};

// 依赖注入装配，自底向上：存储层最长命，接口层最上
std::unique_ptr<AssembledStack> AssembleServices() {
  auto stack = std::make_unique<AssembledStack>();
  stack->gateway = std::make_unique<us3_turbo::proxy::BackendGateway>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms);
  stack->block_storage = std::make_unique<us3_turbo::proxy::BlockStorage>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms,
      FLAGS_backend_block_size_bytes);
  stack->index = std::make_unique<us3_turbo::proxy::InMemoryUploadIndex>();
  auto single_put = std::make_unique<us3_turbo::proxy::SinglePut>(
      stack->gateway.get());
  auto multipart = std::make_unique<us3_turbo::proxy::Multipart>(
      stack->index.get(), stack->block_storage.get());
  stack->service = std::make_unique<us3_turbo::proxy::ProxyService>(
      std::move(single_put), std::move(multipart), stack->index.get());
  return stack;
}

// 注册 service 并启动 brpc server
bool StartServer(brpc::Server& server,
                 us3_turbo::proxy::ProxyService& service) {
  if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG_SYS_ERROR("failed to register control-plane service");
    return false;
  }
  brpc::ServerOptions options;
  options.num_threads = FLAGS_num_threads;
  const std::string endpoint =
      FLAGS_bind_host + ":" + std::to_string(FLAGS_proxy_port);
  if (server.Start(endpoint.c_str(), &options) != 0) {
    LOG_SYS_ERROR("failed to start brpc server on {}", endpoint);
    return false;
  }
  return true;
}

// 阻塞至收到 SIGINT/SIGTERM。
void RunUntilAskedToQuit() {
  sigset_t mask{};
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);
  LOG_SYS_INFO("proxy running, press Ctrl+C to quit");
  int signo = 0;
  sigwait(&mask, &signo);
  LOG_SYS_INFO("received signal {}, shutting down", signo);
}

}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  InitLogging();
  LOG_SYS_INFO("[START] logging initialized (level={})", FLAGS_log_level);

  auto stack = AssembleServices();
  LOG_SYS_INFO("[START] services assembled (backend={})",
               FLAGS_backend_endpoint);

  brpc::Server server;
  if (!StartServer(server, *stack->service)) return EXIT_FAILURE;
  LOG_SYS_INFO("[START] proxy started, listening on {}:{}",
               FLAGS_bind_host, FLAGS_proxy_port);

  RunUntilAskedToQuit();

  server.Stop(0);
  server.Join();
  LOG_SYS_INFO("[STOP] proxy stopped");
  return EXIT_SUCCESS;
}
