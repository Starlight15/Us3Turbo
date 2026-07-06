#include "backend/src/backend_block_data_plane_service.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::backend {

namespace {

// 单 block 字节上限：16 MiB（与单步对象 / 分段 part 上限对齐）。
// proxy 切分产物 ≤4MB，此处为兜底校验，挡住任何超限请求。
constexpr std::uint64_t kMaxBlockBytes = 16ULL * 1024 * 1024;

// block etag：
//   - CRC 开启（完整性态）→ crc32c 的 8 位十六进制（内容派生，可端到端校验）。
//   - CRC 关闭（压测态，默认）→ object_id + ":" + block_size 的占位符。
//     object_id 含 upload_id/part_number/block_no，拼 block_size 后每 block 唯一。
//     【注意】占位符仅保证唯一、供 proxy 汇总 part etag 与 CompleteSession 校验，
//     不承载数据完整性（非内容派生）。
[[nodiscard]] std::string BuildBlockEtag(std::uint32_t crc,
                                         const std::string& object_id,
                                         std::uint64_t block_size) {
  if (crc != 0) {
    char buf[9] = {};
    std::snprintf(buf, sizeof(buf), "%08x", crc);
    return std::string(buf);
  }
  return object_id + ":" + std::to_string(block_size);
}

[[nodiscard]] std::string BuildBlockObjectId(const std::string& upload_id,
                                             std::uint32_t part_number,
                                             std::uint32_t block_no) {
  return upload_id + "/p" + std::to_string(part_number) +
         "/b" + std::to_string(block_no);
}

}  // namespace

BackendBlockDataPlaneService::BackendBlockDataPlaneService(
    BackendGdsSink& sink, rdma::UcxSink& ucx_sink)
    : sink_(sink), ucx_sink_(ucx_sink) {}

void BackendBlockDataPlaneService::PutBlock(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::ProxyBackendPutBlockRequest* request,
    ::us3_turbo::proxy::ProxyBackendPutBlockResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  const std::string object_id = BuildBlockObjectId(
      request->upload_id(), request->part_number(), request->block_no());

  if (request->has_gds_source()) {
    const auto& src = request->gds_source();
    if (src.rdma_token().empty() || src.block_size() == 0) {
      response->set_ok(false);
      response->set_error_message("gds_source incomplete");
      cntl->SetFailed("gds_source incomplete");
      return;
    }
    if (src.block_size() > kMaxBlockBytes) {
      response->set_ok(false);
      response->set_error_message("block_size exceeds 16MiB limit");
      cntl->SetFailed("block_size exceeds 16MiB limit");
      return;
    }
    if (!sink_.available()) {
      response->set_ok(false);
      response->set_error_message("cuObjServer not available");
      cntl->SetFailed("cuObjServer not available");
      return;
    }
    spdlog::info("backend.putblock GDS obj={} token={} off={} size={}",
                 object_id, src.rdma_token(), src.source_offset(),
                 src.block_size());
    auto outcome = sink_.ReceiveAndDiscard(object_id, src.rdma_token(),
                                           src.block_size(),
                                           src.source_offset());
    if (!outcome.ok) {
      response->set_ok(false);
      response->set_error_message("backend: " + outcome.error);
      cntl->SetFailed("backend: " + outcome.error);
      return;
    }
    response->set_ok(true);
    response->set_etag(BuildBlockEtag(outcome.crc32c, object_id, src.block_size()));
    if (outcome.crc32c != 0) response->set_crc32c(outcome.crc32c);
    spdlog::info("backend.putblock GDS obj={} bytes={} crc={:x}",
                 object_id, outcome.bytes_transferred, outcome.crc32c);
    return;
  }

  if (request->has_ucx_source()) {
    const auto& src = request->ucx_source();
    if (src.remote_addr() == 0 || src.packed_rkey().empty() ||
        src.client_ucx_addr().empty() || src.block_size() == 0) {
      response->set_ok(false);
      response->set_error_message("ucx_source incomplete");
      cntl->SetFailed("ucx_source incomplete");
      return;
    }
    if (src.block_size() > kMaxBlockBytes) {
      response->set_ok(false);
      response->set_error_message("block_size exceeds 16MiB limit");
      cntl->SetFailed("block_size exceeds 16MiB limit");
      return;
    }
    if (!ucx_sink_.available()) {
      response->set_ok(false);
      response->set_error_message("UCX worker not available");
      cntl->SetFailed("UCX worker not available");
      return;
    }
    spdlog::info("backend.putblock UCX obj={} addr=0x{:x} size={}",
                 object_id, src.remote_addr(), src.block_size());
    // remote_addr 已由 proxy 加上 source_offset，UCX sink 直接用。
    auto outcome = ucx_sink_.ReceiveAndDiscard(
        object_id, src.client_ucx_addr(), src.remote_addr(),
        src.packed_rkey(), src.block_size());
    if (!outcome.ok) {
      response->set_ok(false);
      response->set_error_message("backend: " + outcome.error);
      cntl->SetFailed("backend: " + outcome.error);
      return;
    }
    response->set_ok(true);
    response->set_etag(BuildBlockEtag(outcome.crc32c, object_id, src.block_size()));
    if (outcome.crc32c != 0) response->set_crc32c(outcome.crc32c);
    spdlog::info("backend.putblock UCX obj={} bytes={} crc={:x}",
                 object_id, outcome.bytes_transferred, outcome.crc32c);
    return;
  }

  response->set_ok(false);
  response->set_error_message("source not set");
  cntl->SetFailed("source not set");
}

}  // namespace us3_turbo::backend
