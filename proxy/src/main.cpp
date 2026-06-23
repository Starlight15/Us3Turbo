#include <csignal>
#include <cstdlib>
#include <string>

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "proxy/src/service/proxy_control_plane_service.h"
#include "proxy/src/session/session_manager.h"

DEFINE_int32(proxy_port, 9100, "proxy control-plane brpc port");
DEFINE_string(bind_host, "192.168.1.198", "Bind host for the brpc listener");
DEFINE_int32(num_threads, 4, "brpc worker thread count");
DEFINE_int64(session_ttl_sec, 300, "Session TTL in seconds");
DEFINE_string(gateway_id, "proxy-0", "Proxy identifier");
DEFINE_string(backend_endpoint, "192.168.1.198:9200",
              "GDS backend data plane endpoint (returned to client in OpenSession)");

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

  us3_turbo::proxy::SessionManager session_mgr(FLAGS_session_ttl_sec);
  us3_turbo::proxy::ProxyControlPlaneService service(
      FLAGS_gateway_id, FLAGS_backend_endpoint, session_mgr);

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
