// ucx_put_channel.cpp — UCX 链路的 PutChannel 实现。
// 搬运自原 client.cpp 的 UcxPutOnce + VerifyUcxCrc32c,逻辑/日志/CRC/trace 不变。

#include "client/src/transport/ucx_put_channel.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "client/src/common/request.h"
#include "client/src/common/crc32c.h"
#include "client/src/rpc/proxy_rpc.h"
#include "client/src/common/trace.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

namespace {

using detail::clk;
using detail::MakeRequestId;
using detail::LatencyStage;
using detail::TraceLatency;

// CRC32C 校验(options.verify_crc32c):UCX 对 host buffer 直算,无需 D2H。
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

// UCX 链路单次尝试:AcquireDescriptor → UcxPut。与 GdsPutChannel 独立,不复用。
bool UcxPutChannel::PutOnce(const ClientProxyPutRequest& request,
                             ConstBufferView buffer,
                             PutPathResult& result) const {
  assert(ucx_mgr_ != nullptr);
  const std::string request_id = MakeRequestId();  // 每次新生成,跨端日志关联

  const bool trace = options_.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  UcxMemoryManager::Descriptor desc;
  if (!ucx_mgr_->AcquireDescriptor(buffer.data, buffer.size, desc)) {  // 懒注册
    return false;
  }
  UcxDataSource ucx_source{desc.remote_addr, desc.rkey, desc.client_ucx_addr};
  auto t_desc = trace ? clk::now() : clk::time_point{};

  if (!proxy_.UcxPut(request_id, request.bucket, request.key, request.object_size,
                     ucx_source, result)) {
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  if (options_.verify_crc32c) {
    if (!VerifyUcxCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  if (trace) {
    const LatencyStage stages[] = {
      {"start", t0}, {"desc", t_desc}, {"put", t_put}
    };
    TraceLatency(request_id, "UcxPut", stages, buffer.size);
  }

  return true;
}

}  // namespace us3_turbo::client
