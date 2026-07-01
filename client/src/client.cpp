#include "us3_turbo/client/client.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "client/src/common/retry_policy.h"
#include "client/src/contracts/put_request.h"
#include "client/src/data/crc32c.h"
#include "client/src/gds_transport/gds_memory_manager.h"
#include "client/src/proxy_rpc.h"
#include "client/src/rdma_transport/ucx_memory_manager.h"

namespace us3_turbo::client {
namespace {

using clk = std::chrono::steady_clock;

constexpr char kNotInitializedMsg[] =
    "Client is not initialized. Call Client::Initialize first.";

// 每次（含每次重试）生成新 request_id，用于跨端日志关联。
[[nodiscard]] std::string MakeRequestId() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::random_device{}()) ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())};
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", rng());
  return std::string("req-") + buf;
}

// ---------------------------------------------------------------------------
//  CRC32C 端到端校验（可选，options.verify_crc32c 开启）
//  两个链路各自一份：gds 需先把 device buffer 拷回 host（D2H），rdma 链路
//  buffer 本就在 host，直接算。日志格式与原内联实现逐字一致，便于解析。
// ---------------------------------------------------------------------------

// GDS 路径：需要 D2H 拷贝后计算 CRC。
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

// UCX 路径：直接对 host buffer 计算 CRC（无需 D2H，ucx 链路的便利）。
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

// ---------------------------------------------------------------------------
//  性能追踪（可选，options.latency_trace 开启）
//  通用：按相邻阶段时间差输出毫秒（3 位小数），总时间 = 首→末时间戳差。
// ---------------------------------------------------------------------------

struct LatencyStage {
  std::string_view    name;
  clk::time_point     timestamp;
};

// 通用的性能追踪函数：stage1/stage2/... 为相邻阶段耗时，total 为首→末总耗时。
void TraceLatency(const std::string& request_id,
                  std::string_view operation_name,
                  std::span<const LatencyStage> stages,
                  std::size_t bytes) {
  const auto ms = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  // 拼接各相邻阶段 "name={:.3f}ms "，原内联格式为 "token={:.3f}ms put={:.3f}ms total=..."。
  std::string parts;
  for (std::size_t i = 1; i < stages.size(); ++i) {
    parts += fmt::format("{}={:.3f}ms ", stages[i].name, ms(stages[i - 1].timestamp,
                                                            stages[i].timestamp));
  }
  const double total =
      stages.size() >= 2 ? ms(stages.front().timestamp, stages.back().timestamp) : 0.0;

  spdlog::info("{} trace (req={}): {}total={:.3f}ms bytes={}",
               operation_name, request_id, parts, total, bytes);
}

[[nodiscard]] bool GdsPutOnce(const ClientOptions& options,
                              const ProxyRpc& proxy,
                              GdsMemoryManager* gds_mgr,
                              const ClientProxyPutRequest& request,
                              ConstBufferView buffer,
                              ClientProxyPutResponse& out) {
  assert(gds_mgr != nullptr);
  // 每次（含每次重试）生成新 request_id，用于跨端日志关联。
  const std::string request_id = MakeRequestId();

  // 1. 性能追踪起点
  const bool trace = options.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  // 2. 获取 RDMA token（device buffer），构造 GDS 数据源
  GdsMemoryManager::Token token;
  if (!gds_mgr->AcquireToken(buffer.data, buffer.size, 0, token)) {
    return false;
  }
  GdsDataSource gds_source{std::string(token.str())};
  auto t_token = trace ? clk::now() : clk::time_point{};

  // 3. 执行 RPC
  PutPathResult result;
  if (!proxy.GdsPut(request_id, request.bucket, request.key, request.object_size,
                    gds_source, result)) {
    out.gds_result = result;
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  // 4. 填充输出结果
  out.gds_result = result;

  // 5. 可选：CRC 校验
  if (options.verify_crc32c) {
    if (!VerifyGdsCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  // 6. 可选：性能追踪
  if (trace) {
    const LatencyStage stages[] = {
      {"start", t0}, {"token", t_token}, {"put", t_put}
    };
    TraceLatency(request_id, "GdsPut", stages, buffer.size);
  }

  return true;
}

// UCX 链路的单次尝试：AcquireDescriptor → UcxPut。
// 与 gds 的 GdsPutOnce 完全独立，不复用。
[[nodiscard]] bool UcxPutOnce(const ClientOptions& options,
                               const ProxyRpc& proxy,
                               UcxMemoryManager* ucx_mgr,
                               const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& out) {
  assert(ucx_mgr != nullptr);
  const std::string request_id = MakeRequestId();

  // 1. 性能追踪起点
  const bool trace = options.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  // 2. 获取 RDMA 描述符（host buffer），构造 UCX 数据源
  UcxMemoryManager::Descriptor desc;
  if (!ucx_mgr->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    return false;
  }
  UcxDataSource ucx_source{desc.remote_addr, desc.rkey, desc.client_ucx_addr};
  auto t_desc = trace ? clk::now() : clk::time_point{};

  // 3. 执行 RPC
  PutPathResult result;
  if (!proxy.UcxPut(request_id, request.bucket, request.key, request.object_size,
                    ucx_source, result)) {
    out.ucx_result = result;
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  // 4. 填充输出结果
  out.ucx_result = result;

  // 5. 可选：CRC 校验（host buffer 直接算，无需 D2H）
  if (options.verify_crc32c) {
    if (!VerifyUcxCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  // 6. 可选：性能追踪
  if (trace) {
    const LatencyStage stages[] = {
      {"start", t0}, {"desc", t_desc}, {"put", t_put}
    };
    TraceLatency(request_id, "UcxPut", stages, buffer.size);
  }

  return true;
}

}  // namespace

Client::Client(ClientOptions options) : options_(std::move(options)) {}
Client::~Client() = default;

bool Client::Initialize() {
  if (initialized_) return true;

  // Mode B：单 channel 指向 proxy，承载 GdsPut / UcxPut。
  // brpc::Channel 线程安全、内部带连接复用，PutObject 重试与 bench 多 worker
  // 共享同一个 Client 时可并发调用。
  proxy_ = std::make_unique<ProxyRpc>(options_.endpoint, options_.default_timeout);
  if (!proxy_->ok()) {
    spdlog::error("Initialize: proxy channel({}) init failed: {}",
                  options_.endpoint, proxy_->init_error());
    proxy_.reset();
    return false;
  }

  if (!GdsMemoryManager::Instance(gds_mgr_)) {
    proxy_.reset();
    return false;
  }

  // UCX 链路 manager。Start 失败不致命：gds 链路仍可用，path=kUcx 的
  // PutObject 会返回 false。仅告警。
  if (!UcxMemoryManager::Instance(ucx_mgr_)) {
    spdlog::warn("Client::Initialize: UCX manager unavailable, "
                 "path=kUcx will fail");
    ucx_mgr_ = nullptr;
  }

  initialized_ = true;
  return true;
}

void Client::Shutdown() {
  proxy_.reset();
  gds_mgr_ = nullptr;
  ucx_mgr_ = nullptr;
  initialized_ = false;
}

bool Client::initialized() const { return initialized_; }

// 公共重试模板：deadline 截止则放弃并记日志，否则交给 ExecuteWithRetry
// 按指数退避重试。模板只在 client.cpp 实例化（PutObject 两条 path 分支），
// 定义置于此唯一编译单元，不违反 ODR。
template <typename PutFunc>
bool Client::ExecutePutWithRetry(const ClientProxyPutRequest& request,
                                 std::string_view method_name,
                                 PutFunc&& put_operation) const {
  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;

  return ExecuteWithRetry(RetryPolicy{}, [&]() -> bool {
    if (std::chrono::steady_clock::now() >= deadline) {
      spdlog::warn("{}: bucket={}/{} retry deadline exceeded",
                   method_name, request.bucket, request.key);
      return false;
    }
    return put_operation();
  });
}

// path 校验：kNone 拒绝（未指定通路），kAll 拒绝（推迟，单 buffer 无法双路）。
// source 不在此检查（由 GdsPutOnce/UcxPutOnce 内部按 path 填充）。
bool Client::ValidatePutPath(const ClientProxyPutRequest& req) const {
  if (req.path == PutDataPath::kNone) {
    spdlog::error("PutObject: path not specified (req={})", req.request_id);
    return false;
  }
  if (HasPath(req.path, PutDataPath::kAll) &&
      req.path == PutDataPath::kAll) {
    spdlog::error("PutObject: kAll not supported yet (req={})", req.request_id);
    return false;
  }
  return true;
}

bool Client::PutObject(const ClientProxyPutRequest& request,
                       ConstBufferView buffer,
                       ClientProxyPutResponse& response) const {
  if (!initialized_) {
    spdlog::error("PutObject: {} (req={})", kNotInitializedMsg, request.request_id);
    return false;
  }
  if (!ValidatePutPath(request)) {
    return false;
  }

  // 大小上限校验（沿用原 put_single_max_bytes）。
  const auto max_put = options_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    spdlog::warn("PutObject: bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
                 "use multipart upload",
                 request.bucket, request.key, buffer.size, max_put);
    return false;
  }

  // 按通路分支：kGds 单路、kUcx 单路（kAll 已在 ValidatePutPath 拒绝）。
  // request_id 由 GdsPutOnce/UcxPutOnce 内部生成，保证每次重试独立。
  bool gds_ok = true;
  bool ucx_ok = true;

  if (HasPath(request.path, PutDataPath::kGds)) {
    if (gds_mgr_ == nullptr) {
      spdlog::error("PutObject: GDS manager not initialized (req={})",
                    request.request_id);
      gds_ok = false;
    } else {
      gds_ok = ExecutePutWithRetry(request, "PutObject", [&]() -> bool {
        return GdsPutOnce(options_, *proxy_, gds_mgr_, request, buffer, response);
      });
      if (!gds_ok) {
        spdlog::warn("PutObject: GDS path failed (req={})",
                     request.request_id);
      }
    }
  }

  if (HasPath(request.path, PutDataPath::kUcx)) {
    if (ucx_mgr_ == nullptr) {
      spdlog::error("PutObject: UCX manager not initialized (req={})",
                    request.request_id);
      ucx_ok = false;
    } else {
      ucx_ok = ExecutePutWithRetry(request, "PutObject", [&]() -> bool {
        return UcxPutOnce(options_, *proxy_, ucx_mgr_, request, buffer, response);
      });
      if (!ucx_ok) {
        spdlog::warn("PutObject: UCX path failed (req={})",
                     request.request_id);
      }
    }
  }

  // kAll 模式（当前不可达）：任意一条成功即可；单路模式下即该路结果。
  return gds_ok || ucx_ok;
}

// UCX 链路的单次尝试：AcquireDescriptor → UcxPut。
// 与 gds 的 GdsPutOnce 完全独立，不复用。
[[nodiscard]] bool UcxPutOnce(const ClientOptions& options,
                               const ProxyRpc& proxy,
                               UcxMemoryManager* ucx_mgr,
                               const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& out) {
  assert(ucx_mgr != nullptr);
  const std::string request_id = MakeRequestId();

  // 1. 性能追踪起点
  const bool trace = options.latency_trace;
  auto t0 = trace ? clk::now() : clk::time_point{};

  // 2. 获取 RDMA 描述符（host buffer），构造 UCX 数据源
  UcxMemoryManager::Descriptor desc;
  if (!ucx_mgr->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    return false;
  }
  UcxDataSource ucx_source{desc.remote_addr, desc.rkey, desc.client_ucx_addr};
  auto t_desc = trace ? clk::now() : clk::time_point{};

  // 3. 执行 RPC
  PutPathResult result;
  if (!proxy.UcxPut(request_id, request.bucket, request.key, request.object_size,
                    ucx_source, result)) {
    out.ucx_result = result;
    return false;
  }
  auto t_put = trace ? clk::now() : clk::time_point{};

  // 4. 填充输出结果
  out.ucx_result = result;

  // 5. 可选：CRC 校验（host buffer 直接算，无需 D2H）
  if (options.verify_crc32c) {
    if (!VerifyUcxCrc32c(request_id, buffer, result.crc32c, request)) {
      return false;
    }
  }

  // 6. 可选：性能追踪
  if (trace) {
    const LatencyStage stages[] = {
      {"start", t0}, {"desc", t_desc}, {"put", t_put}
    };
    TraceLatency(request_id, "UcxPut", stages, buffer.size);
  }

  return true;
}

bool Client::RegisterDeviceBuffer(void* ptr, std::size_t size) {
  if (!initialized_) {
    spdlog::error("RegisterDeviceBuffer: {}", kNotInitializedMsg);
    return false;
  }
  assert(gds_mgr_ != nullptr);
  return gds_mgr_->RegisterBuffer(ptr, size);
}

bool Client::UnregisterDeviceBuffer(void* ptr) {
  if (!initialized_) {
    spdlog::error("UnregisterDeviceBuffer: {}", kNotInitializedMsg);
    return false;
  }
  assert(gds_mgr_ != nullptr);
  return gds_mgr_->UnregisterBuffer(ptr);
}

}  // namespace us3_turbo::client
