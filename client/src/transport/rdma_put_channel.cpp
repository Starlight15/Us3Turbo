// rdma_put_channel.cpp — RDMA 链路的 PutChannel 实现。

#include "client/src/transport/rdma_put_channel.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "client/src/common/crc32c.h"
#include "client/src/common/request.h"
#include "client/src/common/trace.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::client {

namespace {

using detail::clk;
using detail::LatencyStage;
using detail::MakeReqId;
using detail::TraceLatency;

// CRC32C 校验（options.verify_crc32c）：RDMA 对 host buffer 直算，无需 D2H。
[[nodiscard]] bool VerifyRdmaCrc32c(const std::string& req_id, ConstBufferView host_buffer,
                                     std::uint32_t remote_crc32c, const ClientProxyPutRequest& req) {
  const std::uint32_t local = Crc32c(std::span<const std::byte>(
      static_cast<const std::byte*>(host_buffer.data), host_buffer.size));
  const std::uint32_t remote = remote_crc32c;
  if (local == remote) {
    LOG_INFO(req_id, "crc32c MATCH local={:08x} remote={:08x} bucket={}/{} bytes={}", local, remote,
             req.bucket, req.key, host_buffer.size);
    return true;
  }
  LOG_ERROR(req_id, "crc32c MISMATCH local={:08x} remote={:08x} bucket={}/{} bytes={}", local,
            remote, req.bucket, req.key, host_buffer.size);
  return false;
}

}  // namespace

bool RdmaPutChannel::ValidateRdmaRequest(const ClientProxyPutRequest& req,
                                          ConstBufferView buffer) const {
  if (req.path != PutDataPath::kRdma) {
    LOG_SYS_ERROR("RdmaPutChannel: wrong path");
    return false;
  }
  if (buffer.data == nullptr || buffer.size == 0) {
    LOG_SYS_ERROR("RdmaPutChannel: invalid buffer");
    return false;
  }
  return true;
}

// RDMA 链路单次尝试：AcquireDescriptor → RdmaPut。与 GdsPutChannel 独立。
bool RdmaPutChannel::PutOnce(const ClientProxyPutRequest& req, ConstBufferView buffer,
                              PutPathResult& res) const {
  assert(rdma_mgr_ != nullptr);
  const std::string req_id = MakeReqId();  // 每次新生成,跨端日志关联

  if (!ValidateRdmaRequest(req, buffer)) {
    return false;
  }

  const bool trace = opts_.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  RdmaMemoryManager::Descriptor desc;
  if (!rdma_mgr_->AcquireDescriptor(buffer.data, buffer.size, desc)) {  // 懒注册
    return false;
  }
  RdmaDataSource rdma_source{desc.token};
  auto t_desc = trace ? clk::now() : clk::time_point{};

  if (!proxy_.RdmaPut(req_id, req.bucket, req.key, req.object_size, rdma_source, res)) {
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  if (opts_.verify_crc32c) {
    if (!VerifyRdmaCrc32c(req_id, buffer, res.crc32c, req)) {
      return false;
    }
  }

  if (trace) {
    const LatencyStage stages[] = {{"start", t0}, {"desc", t_desc}, {"put", t_put}};
    TraceLatency(req_id, "RdmaPut", stages, buffer.size);
  }

  return true;
}

}  // namespace us3_turbo::client
