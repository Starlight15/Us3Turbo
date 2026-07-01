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

BackendDataPlaneService::BackendDataPlaneService(BackendGdsSink& sink,
                                                 rdma::UcxSink& ucx_sink)
    : sink_(sink), ucx_sink_(ucx_sink) {}

void BackendDataPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // v1 不校验 session/ticket（proxy 已校验）；只要求 source 存在、size 合法。
  if (!request->has_gds_source() ||
      request->gds_source().rdma_token().empty()) {
    response->set_ok(false);
    response->set_error_message("backend v1: missing rdma_token for GdsPut");
    cntl->SetFailed("backend v1: missing rdma_token for GdsPut");
    return;
  }
  // object_size 取自 request；校验在 sink 内部（≤ 1 GiB）。
  const auto chunk_size = request->object_size();
  if (!sink_.available()) {
    response->set_ok(false);
    response->set_error_message("backend v1: cuObjServer not available");
    cntl->SetFailed("backend v1: cuObjServer not available");
    return;
  }

  const std::string object_id =
      BuildObjectId(request->bucket(), request->key());
  const std::string& rdma_token = request->gds_source().rdma_token();

  // 打印 client（经 proxy 转发）传来的 rdma_token（形如 "hexaddr:rkey"），
  // 与 client 侧 AcquireToken 的日志配对，便于跨端核对 token 是否落在
  // 可达 fabric 上。
  spdlog::info("backend.gdsput recv token={} object={} chunk_size={}",
               rdma_token, object_id, chunk_size);

  auto outcome =
      sink_.ReceiveAndDiscard(object_id, rdma_token, chunk_size);
  if (!outcome.ok) {
    response->set_ok(false);
    response->set_error_message("backend v1: " + outcome.error);
    cntl->SetFailed("backend v1: " + outcome.error);
    return;
  }

  // 合成响应：ok + etag + bytes_written + crc32c（供 proxy 转回 client 端到端校验）。
  const std::string etag = BuildEtag(outcome.crc32c);
  response->set_ok(true);
  response->set_etag(etag);
  response->set_bytes_written(outcome.bytes_transferred);
  response->set_crc32c(outcome.crc32c);

  spdlog::info("backend.gdsput object={} bytes={} crc32c={:x}", object_id,
               outcome.bytes_transferred, outcome.crc32c);
}

// ---------------------------------------------------------------------------
// UcxPut（UCX 链路）：委托给 ucx_sink_。与 GdsPut 代码独立、无共享逻辑，
// 仅因 brpc 一个 proto service 只能注册一个 C++ 实例而共处本类。
// ---------------------------------------------------------------------------

void BackendDataPlaneService::UcxPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ClientProxyPutRequest* request,
    ::us3_turbo::proxy::PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  if (!request->has_ucx_source()) {
    response->set_ok(false);
    response->set_error_message("backend v1: missing ucx_source for UcxPut");
    cntl->SetFailed("backend v1: missing ucx_source for UcxPut");
    return;
  }
  const auto& src = request->ucx_source();
  if (src.remote_addr() == 0) {
    response->set_ok(false);
    response->set_error_message("backend v1: missing remote_addr for UcxPut");
    cntl->SetFailed("backend v1: missing remote_addr for UcxPut");
    return;
  }
  if (src.packed_rkey().empty()) {
    response->set_ok(false);
    response->set_error_message("backend v1: missing packed_rkey for UcxPut");
    cntl->SetFailed("backend v1: missing packed_rkey for UcxPut");
    return;
  }
  if (src.client_ucx_addr().empty()) {
    response->set_ok(false);
    response->set_error_message("backend v1: missing client_ucx_addr for UcxPut");
    cntl->SetFailed("backend v1: missing client_ucx_addr for UcxPut");
    return;
  }
  if (!ucx_sink_.available()) {
    response->set_ok(false);
    response->set_error_message("backend v1: UCX worker not available");
    cntl->SetFailed("backend v1: UCX worker not available");
    return;
  }

  const std::string object_id = BuildObjectId(request->bucket(), request->key());
  const auto chunk_size = request->object_size();

  spdlog::info("backend.ucxput recv object={} chunk_size={} "
               "remote_addr=0x{:x} rkey_bytes={} client_ucx_addr={}",
               object_id, chunk_size, src.remote_addr(),
               src.packed_rkey().size(), src.client_ucx_addr());

  auto outcome = ucx_sink_.ReceiveAndDiscard(
      object_id, src.client_ucx_addr(), src.remote_addr(),
      src.packed_rkey(), chunk_size);
  if (!outcome.ok) {
    response->set_ok(false);
    response->set_error_message("backend v1: " + outcome.error);
    cntl->SetFailed("backend v1: " + outcome.error);
    return;
  }

  const std::string etag = BuildEtag(outcome.crc32c);
  response->set_ok(true);
  response->set_etag(etag);
  response->set_bytes_written(outcome.bytes_transferred);
  response->set_crc32c(outcome.crc32c);

  spdlog::info("backend.ucxput object={} bytes={} crc32c={:x}", object_id,
               outcome.bytes_transferred, outcome.crc32c);
}

}  // namespace us3_turbo::backend
