#include "backend/src/backend_data_plane_service.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::backend {

namespace {

constexpr std::string_view kNotImplemented =
    "backend v1: only single-object GdsPut";

[[nodiscard]] std::string BuildEtag(std::uint32_t crc) {
  char buf[9] = {};
  std::snprintf(buf, sizeof(buf), "%08x", crc);
  return std::string(buf);
}

[[nodiscard]] std::string BuildObjectId(const std::string& bucket,
                                        const std::string& object_key) {
  return bucket + "/" + object_key;
}

}  // namespace

BackendDataPlaneService::BackendDataPlaneService(BackendGdsSink& sink)
    : sink_(sink) {}

void BackendDataPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::GdsChunkRequest* request,
    ::us3_turbo::proxy::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // v1 不校验 session/ticket（proxy 已校验）；只要求 token 非空、长度合法。
  if (request->rdma_token().empty()) {
    cntl->SetFailed("backend v1: missing rdma_token for GdsPut");
    return;
  }
  // chunk_size 取自 request；校验在 sink 内部（≤ 1 GiB）。
  const auto chunk_size = request->chunk_size();
  if (!sink_.available()) {
    cntl->SetFailed("backend v1: cuObjServer not available");
    return;
  }

  const std::string object_id =
      BuildObjectId(request->bucket(), request->object_key());

  // 打印 client（经 proxy 转发）传来的 rdma_token（形如 "hexaddr:rkey"），
  // 与 client 侧 AcquireToken 的日志配对，便于跨端核对 token 是否落在
  // 可达 fabric 上。
  spdlog::info("backend.gdsput recv token={} object={} chunk_size={}",
               request->rdma_token(), object_id, chunk_size);

  auto outcome =
      sink_.ReceiveAndDiscard(object_id, request->rdma_token(), chunk_size);
  if (!outcome.ok) {
    cntl->SetFailed("backend v1: " + outcome.error);
    return;
  }

  // 合成响应：etag + bytes_received + crc32c（供 proxy 转回 client 端到端校验）。
  const std::string etag = BuildEtag(outcome.crc32c);
  response->set_etag(etag);
  response->set_bytes_received(outcome.bytes_transferred);
  response->set_crc32c(outcome.crc32c);

  spdlog::info("backend.gdsput object={} bytes={} crc32c={:x}", object_id,
               outcome.bytes_transferred, outcome.crc32c);
}

// ---------------------------------------------------------------------------
// v1 占位方法：一律返回 "backend v1: only single-object GdsPut"
// ---------------------------------------------------------------------------

void BackendDataPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::OpenSessionRequest* /*request*/,
    ::us3_turbo::proxy::OpenSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void BackendDataPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortSessionRequest* /*request*/,
    ::us3_turbo::proxy::AbortSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

}  // namespace us3_turbo::backend
