#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "proxy/src/api/proxy_control_plane_service.h"
#include "proxy/src/index/in_memory_upload_index.h"
#include "proxy/src/service/multipart_service.h"
#include "proxy/src/service/single_put_service.h"
#include "proxy/src/storage/backend_gateway.h"
#include "proxy/src/storage/block_storage.h"

DEFINE_int32(proxy_port, 9100, "proxy control-plane brpc port");
DEFINE_string(bind_host, "192.168.1.198", "Bind host for the brpc listener");
DEFINE_int32(num_threads, 4, "brpc worker thread count");
DEFINE_string(backend_endpoint, "192.168.1.198:9200",
              "backend data plane endpoint (GdsPut/UcxPut/PutBlock)");
DEFINE_int32(backend_timeout_ms, 30000,
             "Timeout (ms) for proxy→backend forward (GdsPut/UcxPut/PutBlock)");

namespace {

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

  // 依赖注入装配：自底向上，存储层最长命（栈底），接口层最上，析构逆序安全。
  auto gateway       = std::make_unique<us3_turbo::proxy::BackendGateway>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms);
  auto block_storage = std::make_unique<us3_turbo::proxy::BlockStorage>(
      FLAGS_backend_endpoint, FLAGS_backend_timeout_ms);
  auto index         = std::make_unique<us3_turbo::proxy::InMemoryUploadIndex>();
  auto single_svc    = std::make_unique<us3_turbo::proxy::SinglePutService>(
      gateway.get());
  auto multipart_svc = std::make_unique<us3_turbo::proxy::MultipartService>(
      index.get(), block_storage.get());

  us3_turbo::proxy::ProxyControlPlaneService service(
      std::move(single_svc), std::move(multipart_svc), index.get());

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
