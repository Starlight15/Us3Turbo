#include "proxy/src/multipart/multipart_put_handler.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <brpc/controller.h>
#include <spdlog/spdlog.h>

#include "proxy/src/common/utils.h"

namespace us3_turbo::proxy {

MultipartPutHandler::MultipartPutHandler(
    ::us3_turbo::proxy::BackendDataPlane_Stub* backend_stub,
    int timeout_ms,
    std::uint64_t block_size)
    : backend_stub_(backend_stub),
      timeout_ms_(timeout_ms),
      block_size_(block_size == 0 ? 4ULL * 1024 * 1024 : block_size) {}

std::vector<MultipartPutHandler::BlockPlan>
MultipartPutHandler::SplitToBlocks(std::uint64_t part_size) const {
  std::vector<BlockPlan> blocks;
  if (part_size == 0) return blocks;
  std::uint64_t offset = 0;
  std::uint32_t no = 0;
  while (offset < part_size) {
    const std::uint64_t remaining = part_size - offset;
    const std::uint64_t sz = std::min(remaining, block_size_);
    blocks.push_back({no++, offset, sz});
    offset += sz;
  }
  return blocks;
}

::us3_turbo::proxy::ProxyBackendPutBlockResponse
MultipartPutHandler::CallBackendPutBlockGds(
    const std::string& request_id,
    const std::string& upload_id,
    std::uint32_t part_number,
    const BlockPlan& block,
    const std::string& rdma_token) {
  ::us3_turbo::proxy::ProxyBackendPutBlockRequest req;
  req.set_request_id(request_id);
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_block_no(block.block_no);
  auto* src = req.mutable_gds_source();
  src->set_rdma_token(rdma_token);
  src->set_source_offset(block.source_offset);  // 关键：透传 offset
  src->set_block_size(block.block_size);

  ::us3_turbo::proxy::ProxyBackendPutBlockResponse resp;
  brpc::Controller cntl;
  cntl.set_timeout_ms(timeout_ms_);
  backend_stub_->PutBlock(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) {
    resp.set_ok(false);
    resp.set_error_message(std::string("PutBlock rpc failed: ") +
                           cntl.ErrorText());
  }
  return resp;
}

::us3_turbo::proxy::ProxyBackendPutBlockResponse
MultipartPutHandler::CallBackendPutBlockUcx(
    const std::string& request_id,
    const std::string& upload_id,
    std::uint32_t part_number,
    const BlockPlan& block,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr) {
  ::us3_turbo::proxy::ProxyBackendPutBlockRequest req;
  req.set_request_id(request_id);
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_block_no(block.block_no);
  auto* src = req.mutable_ucx_source();
  src->set_remote_addr(remote_addr + block.source_offset);  // 地址加偏移
  src->set_packed_rkey(packed_rkey);
  src->set_client_ucx_addr(client_ucx_addr);
  src->set_block_size(block.block_size);

  ::us3_turbo::proxy::ProxyBackendPutBlockResponse resp;
  brpc::Controller cntl;
  cntl.set_timeout_ms(timeout_ms_);
  backend_stub_->PutBlock(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) {
    resp.set_ok(false);
    resp.set_error_message(std::string("PutBlock rpc failed: ") +
                           cntl.ErrorText());
  }
  return resp;
}

MultipartPutHandler::PartResult MultipartPutHandler::Aggregate(
    const std::vector<std::pair<
        BlockPlan, ::us3_turbo::proxy::ProxyBackendPutBlockResponse>>& results,
    std::uint64_t part_size) {
  PartResult r;
  r.bytes_written = part_size;

  // 1. 任一 block 失败 → part 失败（取第一个失败原因）。
  for (const auto& [plan, resp] : results) {
    if (!resp.ok()) {
      r.ok = false;
      r.error = "block " + std::to_string(plan.block_no) +
                " failed: " + resp.error_message();
      return r;
    }
  }

  // 2. 按 block_no 排序后汇总 etag。
  auto sorted = results;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
              return a.first.block_no < b.first.block_no;
            });
  std::vector<std::string> etags;
  etags.reserve(sorted.size());
  for (const auto& [_, resp] : sorted) etags.push_back(resp.etag());

  r.etag = utils::CombineETags(etags);
  if (etags.size() == 1 && sorted[0].second.has_crc32c()) {
    r.crc32c = sorted[0].second.crc32c();
  }
  // 多 block 不汇总 crc（block crc 仅做传输校验，不合成 part crc）。
  r.ok = true;
  return r;
}

MultipartPutHandler::PartResult MultipartPutHandler::HandleGdsPart(
    const std::string& request_id,
    const std::string& upload_id,
    std::uint32_t part_number,
    std::uint64_t part_size,
    const std::string& rdma_token) {
  if (backend_stub_ == nullptr) {
    return {false, "", "backend block stub not available", 0, 0};
  }
  const auto blocks = SplitToBlocks(part_size);
  spdlog::info("HandleGdsPart: req={} upload={} part={} size={} blocks={}",
               request_id, upload_id, part_number, part_size, blocks.size());

  // 串行调用各 block（block 数 ≤4，串行简单、无线程开销）。
  std::vector<std::pair<BlockPlan, ::us3_turbo::proxy::ProxyBackendPutBlockResponse>>
      results;
  results.reserve(blocks.size());
  for (const auto& block : blocks) {
    results.emplace_back(block,
        CallBackendPutBlockGds(request_id, upload_id, part_number,
                               block, rdma_token));
  }

  auto r = Aggregate(results, part_size);
  spdlog::info("HandleGdsPart done: req={} part={} ok={} etag={} crc={:x}",
               request_id, part_number, r.ok, r.etag, r.crc32c);
  return r;
}

MultipartPutHandler::PartResult MultipartPutHandler::HandleUcxPart(
    const std::string& request_id,
    const std::string& upload_id,
    std::uint32_t part_number,
    std::uint64_t part_size,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr) {
  if (backend_stub_ == nullptr) {
    return {false, "", "backend block stub not available", 0, 0};
  }
  const auto blocks = SplitToBlocks(part_size);
  spdlog::info("HandleUcxPart: req={} upload={} part={} size={} blocks={}",
               request_id, upload_id, part_number, part_size, blocks.size());

  // 串行调用各 block（block 数 ≤4，串行简单、无线程开销）。
  std::vector<std::pair<BlockPlan, ::us3_turbo::proxy::ProxyBackendPutBlockResponse>>
      results;
  results.reserve(blocks.size());
  for (const auto& block : blocks) {
    results.emplace_back(block,
        CallBackendPutBlockUcx(request_id, upload_id, part_number,
                               block, remote_addr, packed_rkey,
                               client_ucx_addr));
  }

  auto r = Aggregate(results, part_size);
  spdlog::info("HandleUcxPart done: req={} part={} ok={} etag={} crc={:x}",
               request_id, part_number, r.ok, r.etag, r.crc32c);
  return r;
}

}  // namespace us3_turbo::proxy
