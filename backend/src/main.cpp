#include <csignal>
#include <cstdlib>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "backend/src/backend_data_plane_service.h"
#include "backend/src/backend_gds_sink.h"

DEFINE_int32(backend_brpc_port, 9200, "backend GdsPut brpc port");
DEFINE_int32(backend_rdma_port, 18516,
             "cuObjServer RDMA listener port (matches gateway default)");
DEFINE_string(bind_host, "192.168.1.198", "Bind host for brpc and cuObjServer");
DEFINE_string(public_host, "192.168.1.198", "Public host (unused in v1)");
DEFINE_int32(num_threads, 4, "brpc worker thread count");
DEFINE_string(backend_id, "backend-0", "Backend identifier");
DEFINE_string(proxy_endpoint, "192.168.1.198:9100",
              "Proxy control plane endpoint for completion notification "
              "(empty disables ReportGdsPut)");

namespace {

void RunUntilAskedToQuit() {
  sigset_t mask{};
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);
  spdlog::info("backend running, press Ctrl+C to quit");
  int signo = 0;
  sigwait(&mask, &signo);
  spdlog::info("received signal {}, shutting down", signo);
}

}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // 启动顺序：先 cuObjServer + PinnedBufferPool，失败即退出。
  us3_turbo::backend::BackendGdsSink sink(FLAGS_bind_host,
                                                  FLAGS_backend_rdma_port);
  if (!sink.Start()) {
    spdlog::error("backend: failed to start cuObjServer, exiting");
    return EXIT_FAILURE;
  }

  us3_turbo::backend::BackendDataPlaneService service(sink,
                                                             FLAGS_backend_id,
                                                             FLAGS_proxy_endpoint);

  brpc::Server server;
  if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    spdlog::error("backend: failed to register data-plane service");
    return EXIT_FAILURE;
  }

  brpc::ServerOptions options;
  options.num_threads = FLAGS_num_threads;

  const std::string endpoint =
      FLAGS_bind_host + ":" + std::to_string(FLAGS_backend_brpc_port);
  if (server.Start(endpoint.c_str(), &options) != 0) {
    spdlog::error("backend: failed to start brpc server on {}", endpoint);
    return EXIT_FAILURE;
  }

  spdlog::info("backend: brpc on {}", endpoint);
  RunUntilAskedToQuit();

  server.Stop(0);
  server.Join();
  sink.Stop();
  spdlog::info("backend stopped");
  return EXIT_SUCCESS;
}
