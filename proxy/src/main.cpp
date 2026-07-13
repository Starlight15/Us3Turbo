#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "proxy/src/api/proxy_service.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/index/dbgate_client.h"
#include "proxy/src/index/mongo_upload_index.h"
#include "proxy/src/logging/access_logger.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/service/get_object.h"
#include "proxy/src/service/multipart.h"
#include "proxy/src/service/single_put.h"
#include "proxy/src/storage/ufile_ac_client.h"

namespace {

/* 初始化日志 */
void InitLogging() {
  spdlog::level::level_enum level = spdlog::level::info;
  if (FLAGS_log_level == "debug")
    level = spdlog::level::debug;
  else if (FLAGS_log_level == "warn")
    level = spdlog::level::warn;
  else if (FLAGS_log_level == "error")
    level = spdlog::level::err;
  us3_turbo::proxy::Logger::Init(
      level, static_cast<std::size_t>(FLAGS_log_max_size_mb),
      static_cast<std::size_t>(FLAGS_log_max_files));
  us3_turbo::proxy::AccessLogger::Instance();
}

/* 依赖注入装配，自底向上：存储层最长命，接口层最上 */
std::unique_ptr<us3_turbo::proxy::AssembledStack> AssembleServices() {
  auto stack = std::make_unique<us3_turbo::proxy::AssembledStack>();
  stack->ufile_ac = std::make_unique<us3_turbo::proxy::UfileAcClient>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms);
  stack->dbgate = std::make_unique<us3_turbo::proxy::DBGateClient>(
      FLAGS_dbgate_endpoint, FLAGS_dbgate_timeout_ms,
      FLAGS_dbgate_conn_pool_size);
  stack->index =
      std::make_unique<us3_turbo::proxy::MongoUploadIndex>(stack->dbgate.get());
  auto single_put = std::make_unique<us3_turbo::proxy::SinglePut>(
      stack->index.get(), stack->ufile_ac.get());
  auto multipart = std::make_unique<us3_turbo::proxy::Multipart>(
      stack->index.get(), stack->ufile_ac.get());
  auto get_object = std::make_unique<us3_turbo::proxy::GetObject>(
      stack->index.get(), stack->ufile_ac.get());
  stack->service = std::make_unique<us3_turbo::proxy::ProxyService>(
      std::move(single_put), std::move(multipart), std::move(get_object),
      stack->index.get());
  return stack;
}

/* 注册 service 并启动 brpc server */
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

/* 阻塞至收到 SIGINT/SIGTERM */
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
  LOG_SYS_INFO("[START] services assembled");
  LOG_SYS_INFO(
      "[CONFIG] backend_endpoint={} (ufile-ac TCP, single-step + multipart "
      "blocks)",
      FLAGS_backend_endpoint);

  brpc::Server server;
  if (!StartServer(server, *stack->service)) return EXIT_FAILURE;
  LOG_SYS_INFO("[START] proxy started, listening on {}:{}", FLAGS_bind_host,
               FLAGS_proxy_port);

  RunUntilAskedToQuit();

  server.Stop(0);
  server.Join();
  LOG_SYS_INFO("[STOP] proxy stopped");
  return EXIT_SUCCESS;
}
