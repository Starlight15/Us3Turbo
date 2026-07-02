// gds_put_channel.cpp — GDS 链路的 PutChannel 实现。
//
// 逐字搬运自原 client.cpp 的 GdsPutOnce + VerifyGdsCrc32c,逻辑不变
// (日志文本 / CRC 行为 / trace 格式全部与重构前一致)。迁入 channel 类后,
// MakeRequestId / TraceLatency / LatencyStage 取自共享 put_trace.h
// (见 review/client_refactor_prompt.md 阶段1.3)。

#include "client/src/transport/gds_put_channel.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "client/src/contracts/put_request.h"
#include "client/src/data/crc32c.h"
#include "client/src/proxy_rpc.h"
#include "client/src/transport/put_trace.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

namespace {

using detail::clk;
using detail::MakeRequestId;
using detail::LatencyStage;
using detail::TraceLatency;

// ---------------------------------------------------------------------------
//  CRC32C 端到端校验（可选，options.verify_crc32c 开启）
//  GDS 路径：需要 D2H 拷贝后计算 CRC。原 client.cpp 私有实现,逐字搬运。
// ---------------------------------------------------------------------------
[[nodiscard]] bool VerifyGdsCrc32c(const std::string& request_id,
                                   ConstBufferView device_buffer,
                                   std::uint32_t remote_crc32c,
                                   const ClientProxyPutRequest& request) {
  std::vector<std::byte> host(device_buffer.size);
  if (cudaError_t e = cudaMemcpy(host.data(), device_buffer.data,
                                 device_buffer.size, cudaMemcpyDeviceToHost);
      e != cudaSuccess) {
    spdlog::error("GdsPut (req={}): verify_crc32c D2H copy failed: {}",
                  request_id, cudaGetErrorString(e));
    return false;
  }
  const std::uint32_t local =
      Crc32c(std::span<const std::byte>(host.data(), host.size()));
  const std::uint32_t remote = remote_crc32c;
  if (local == remote) {
    spdlog::info("GdsPut (req={}): crc32c MATCH local={:08x} remote={:08x} "
                 "bucket={}/{} bytes={}",
                 request_id, local, remote, request.bucket, request.key,
                 device_buffer.size);
    return true;
  }
  spdlog::error("GdsPut (req={}): crc32c MISMATCH local={:08x} remote={:08x} "
                "bucket={}/{} bytes={}",
                request_id, local, remote, request.bucket, request.key,
                device_buffer.size);
  return false;
}

}  // namespace

bool GdsPutChannel::PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const {
  assert(gds_mgr_ != nullptr);
  // 每次（含每次重试）生成新 request_id，用于跨端日志关联。
  const std::string request_id = MakeRequestId();

  // 1. 性能追踪起点
  const bool trace = options_.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  // 2. 获取 RDMA token（device buffer），构造 GDS 数据源
  GdsMemoryManager::Token token;
  if (!gds_mgr_->AcquireToken(buffer.data, buffer.size, 0, token)) {
    return false;
  }
  GdsDataSource gds_source{std::string(token.str())};
  auto t_token = trace ? clk::now() : clk::time_point{};

  // 3. 执行 RPC
  if (!proxy_.GdsPut(request_id, request.bucket, request.key, request.object_size,
                     gds_source, result)) {
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  // 4. 可选：CRC 校验
  if (options_.verify_crc32c) {
    if (!VerifyGdsCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  // 5. 可选：性能追踪
  if (trace) {
    const LatencyStage stages[] = {
      {"start", t0}, {"token", t_token}, {"put", t_put}
    };
    TraceLatency(request_id, "GdsPut", stages, buffer.size);
  }

  return true;
}

}  // namespace us3_turbo::client
