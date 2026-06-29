#include <csignal>
#include <cstdlib>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "backend/src/backend_data_plane_service.h"
#include "backend/src/backend_gds_sink.h"
#include "backend/src/rdma/ucx_sink.h"

DEFINE_int32(backend_brpc_port, 9200, "backend control-plane brpc port");
DEFINE_int32(backend_rdma_port, 18516,
             "cuObjServer RDMA listener port (matches gateway default)");
DEFINE_string(bind_host, "192.168.1.198", "Bind host for brpc and cuObjServer");
DEFINE_string(public_host, "192.168.1.198", "Public host (unused in v1)");
DEFINE_int32(num_threads, 4, "brpc worker thread count");
DEFINE_string(backend_id, "backend-0", "Backend identifier");
DEFINE_bool(backend_compute_crc32c, true,
            "Compute CRC32C over received bytes in the GDS/UCX sinks (for "
            "end-to-end verification). Turn off to skip the scan and "
            "measure raw transfer throughput (crc32c/etag then 0).");

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
                                                  FLAGS_backend_rdma_port,
                                                  FLAGS_backend_compute_crc32c);
  if (!sink.Start()) {
    spdlog::error("backend: failed to start cuObjServer, exiting");
    return EXIT_FAILURE;
  }

  // UCX rdma 链路 sink（与 gds sink 独立）。Start 失败不致命：gds 链路仍可用，
  // 但 RdmaPut 会拒绝。日志告警，不退出。
  us3_turbo::backend::rdma::UcxSink ucx_sink(FLAGS_backend_compute_crc32c);
  if (!ucx_sink.Start()) {
    spdlog::warn("backend: UCX sink start failed, RdmaPut will be unavailable");
  }

  // gds 与 rdma 两条链路共处一个 BackendDataPlaneService：brpc 一个 proto
  // service 只能注册一个 C++ 实例（按 service descriptor full_name 去重），
  // 故 RdmaPut 与 GdsPut 由同一对象持有，但内部代码独立、无共享逻辑。
  us3_turbo::backend::BackendDataPlaneService service(sink, ucx_sink);

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
  ucx_sink.Stop();
  sink.Stop();
  spdlog::info("backend stopped");
  return EXIT_SUCCESS;
}
