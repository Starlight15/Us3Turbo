// gds_put_channel.cpp — GDS 链路的 PutChannel 实现。

#include "client/src/transport/gds_put_channel.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "client/src/common/crc32c.h"
#include "client/src/common/request.h"
#include "client/src/common/trace.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

#include <cuda_runtime.h>

namespace us3_turbo::client {

namespace {

using detail::clk;
using detail::LatencyStage;
using detail::MakeRequestId;
using detail::TraceLatency;

/**
 * @brief CRC32C 校验（options.verify_crc32c）：GDS 需 D2H 拷贝后计算。
 */
[[nodiscard]] bool VerifyGdsCrc32c(const std::string& request_id,
                                   ConstBufferView device_buffer,
                                   std::uint32_t remote_crc32c,
                                   const ClientProxyPutRequest& request) {
  std::vector<std::byte> host(device_buffer.size);
  cudaError_t e = cudaMemcpy(host.data(), device_buffer.data,
                             device_buffer.size, cudaMemcpyDeviceToHost);
  if (e != cudaSuccess) {
    spdlog::error("GdsPut (req={}): verify_crc32c D2H copy failed: {}",
                  request_id, cudaGetErrorString(e));
    return false;
  }
  const std::uint32_t local =
      Crc32c(std::span<const std::byte>(host.data(), host.size()));
  const std::uint32_t remote = remote_crc32c;
  if (local == remote) {
    spdlog::info(
        "GdsPut (req={}): crc32c MATCH local={:08x} remote={:08x} "
        "bucket={}/{} bytes={}",
        request_id, local, remote, request.bucket, request.key,
        device_buffer.size);
    return true;
  }
  spdlog::error(
      "GdsPut (req={}): crc32c MISMATCH local={:08x} remote={:08x} "
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
  const std::string request_id = MakeRequestId();  // 每次新生成,跨端日志关联

  const bool trace = opts_.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  GdsMemoryManager::Token token;
  if (!gds_mgr_->AcquireToken(buffer.data, buffer.size, 0, token)) {  // 懒注册
    return false;
  }
  GdsDataSource gds_source{std::string(token.str())};
  auto t_token = trace ? clk::now() : clk::time_point{};

  if (!proxy_.GdsPut(request_id, request.bucket, request.key,
                     request.object_size, gds_source, result)) {
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  if (opts_.verify_crc32c) {
    if (!VerifyGdsCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  if (trace) {
    const LatencyStage stages[] = {
        {"start", t0}, {"token", t_token}, {"put", t_put}};
    TraceLatency(request_id, "GdsPut", stages, buffer.size);
  }

  return true;
}

}  // namespace us3_turbo::client
