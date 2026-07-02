// ucx_put_channel.cpp — UCX 链路的 PutChannel 实现。
//
// 逐字搬运自原 client.cpp 的 UcxPutOnce + VerifyUcxCrc32c,逻辑不变
// (日志文本 / CRC 行为 / trace 格式全部与重构前一致)。
// **去重**:原 client.cpp 的 UcxPutOnce 被定义了两次(L192 与 L385),
// 本次重构只保留迁入本文件的一份(见 review/client_refactor_prompt.md
// 阶段1.4)。

#include "client/src/transport/ucx_put_channel.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

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
//  UCX 路径：直接对 host buffer 计算 CRC（无需 D2H，ucx 链路的便利）。
//  原 client.cpp 私有实现,逐字搬运。
// ---------------------------------------------------------------------------
[[nodiscard]] bool VerifyUcxCrc32c(const std::string& request_id,
                                    ConstBufferView host_buffer,
                                    std::uint32_t remote_crc32c,
                                    const ClientProxyPutRequest& request) {
  const std::uint32_t local = Crc32c(
      std::span<const std::byte>(static_cast<const std::byte*>(host_buffer.data),
                                 host_buffer.size));
  const std::uint32_t remote = remote_crc32c;
  if (local == remote) {
    spdlog::info("UcxPut (req={}): crc32c MATCH local={:08x} remote={:08x} "
                 "bucket={}/{} bytes={}",
                 request_id, local, remote, request.bucket, request.key,
                 host_buffer.size);
    return true;
  }
  spdlog::error("UcxPut (req={}): crc32c MISMATCH local={:08x} remote={:08x} "
                "bucket={}/{} bytes={}",
                request_id, local, remote, request.bucket, request.key,
                host_buffer.size);
  return false;
}

}  // namespace

// UCX 链路的单次尝试：AcquireDescriptor → UcxPut。
// 与 gds 的 GdsPutChannel::PutOnce 完全独立，不复用。
bool UcxPutChannel::PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const {
  assert(ucx_mgr_ != nullptr);
  const std::string request_id = MakeRequestId();

  // 1. 性能追踪起点
  const bool trace = options_.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  // 2. 获取 RDMA 描述符（host buffer），构造 UCX 数据源
  UcxMemoryManager::Descriptor desc;
  if (!ucx_mgr_->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    return false;
  }
  UcxDataSource ucx_source{desc.remote_addr, desc.rkey, desc.client_ucx_addr};
  auto t_desc = trace ? clk::now() : clk::time_point{};

  // 3. 执行 RPC
  if (!proxy_.UcxPut(request_id, request.bucket, request.key, request.object_size,
                     ucx_source, result)) {
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  // 4. 可选：CRC 校验（host buffer 直接算，无需 D2H）
  if (options_.verify_crc32c) {
    if (!VerifyUcxCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  // 5. 可选：性能追踪
  if (trace) {
    const LatencyStage stages[] = {
      {"start", t0}, {"desc", t_desc}, {"put", t_put}
    };
    TraceLatency(request_id, "UcxPut", stages, buffer.size);
  }

  return true;
}

}  // namespace us3_turbo::client
