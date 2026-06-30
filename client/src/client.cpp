#include "us3_turbo/client/client.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include <spdlog/spdlog.h>

#include "client/src/contracts/requests.h"
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

// per-attempt 超时：request.timeout > 0 则用之，否则回退到 options.request_timeout。
[[nodiscard]] std::chrono::milliseconds ResolveTimeout(
    const ClientOptions& options, const PutObjectRequest& request) {
  return (request.timeout.count() > 0) ? request.timeout : options.request_timeout;
}

struct RetryPolicy {
  int                       max_attempts{3};
  std::chrono::milliseconds initial_backoff{std::chrono::milliseconds(100)};
  std::chrono::milliseconds max_backoff{std::chrono::milliseconds(2000)};
};

// Fn: () -> bool (true = success)
template <typename Fn>
bool RetryIfRetryable(const RetryPolicy& policy, Fn&& fn) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> jitter(0.5, 1.5);

  bool ok = fn();
  for (int attempt = 1; attempt < policy.max_attempts; ++attempt) {
    if (ok) return true;
    auto backoff = std::chrono::milliseconds{
        static_cast<long long>(
            policy.initial_backoff.count() * (1LL << (attempt - 1)))};
    if (backoff > policy.max_backoff) backoff = policy.max_backoff;
    backoff = std::chrono::milliseconds{
        static_cast<long long>(static_cast<double>(backoff.count()) * jitter(rng))};
    std::this_thread::sleep_for(backoff);
    ok = fn();
  }
  return ok;
}

[[nodiscard]] bool PutOnce(const ClientOptions& options,
                          const ProxyRpc& proxy,
                          GdsMemoryManager* gds_mgr,
                          const PutObjectRequest& request,
                          ConstBufferView buffer,
                          TransferOutcome& out) {
  assert(gds_mgr != nullptr);
  const std::string request_id = MakeRequestId();
  const auto timeout = ResolveTimeout(options, request);

  const bool trace = options.latency_trace;
  auto t0 = clk::now();

  GdsMemoryManager::Token token;
  if (!gds_mgr->AcquireToken(buffer.data, buffer.size, 0, token)) {
    return false;
  }
  auto t_token = clk::now();

  GdsPutResult result;
  if (!proxy.Put(request, request_id, timeout, token.str(), buffer.size, result)) {
    return false;
  }
  auto t_put = clk::now();

  out.bytes_transferred = buffer.size;
  out.request_id        = request_id;
  out.etag              = result.etag;
  out.crc32c            = result.crc32c;

  // 端到端 CRC32C 一致性校验(可选):把 device buffer 拷回 host 计算本地 CRC32C,
  // 与 backend 在 GdsChunkResponse.crc32c 回传的值比对。不一致视为失败(返回 false,
  // 上层 RetryIfRetryable 会重试),并记录 MISMATCH 日志。
  if (options.verify_crc32c) {
    std::vector<std::byte> host(buffer.size);
    if (cudaError_t e = cudaMemcpy(host.data(), buffer.data, buffer.size,
                                   cudaMemcpyDeviceToHost); e != cudaSuccess) {
      spdlog::error("GdsPut (req={}): verify_crc32c D2H copy failed: {}",
                    request_id, cudaGetErrorString(e));
      return false;
    }
    const std::uint32_t local = Crc32c(std::span<const std::byte>(host.data(), host.size()));
    const std::uint32_t remote = result.crc32c;
    if (local == remote) {
      spdlog::info("GdsPut (req={}): crc32c MATCH local={:08x} remote={:08x} "
                   "bucket={}/{} bytes={}",
                   request_id, local, remote, request.bucket, request.key,
                   buffer.size);
    } else {
      spdlog::error("GdsPut (req={}): crc32c MISMATCH local={:08x} remote={:08x} "
                    "bucket={}/{} bytes={}",
                    request_id, local, remote, request.bucket, request.key,
                    buffer.size);
      return false;
    }
  }

  if (trace) {
    const auto ms = [](clk::time_point a, clk::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    spdlog::info("GdsPut trace (req={}): token={:.3f}ms "
                 "put={:.3f}ms total={:.3f}ms bytes={}",
                 request_id, ms(t0, t_token),
                 ms(t_token, t_put), ms(t0, t_put), buffer.size);
  }

  return true;
}

}  // namespace

Client::Client(ClientOptions options) : options_(std::move(options)) {}
Client::~Client() = default;

bool Client::Initialize() {
  if (initialized_) return true;

  // Mode B：单 channel 指向 proxy，承载 GdsPut / RdmaPut。
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

  // UCX rdma 链路 manager。Start 失败不致命：gds 链路仍可用，PutObjectRdma
  // 会返回 false。仅告警。
  if (!UcxMemoryManager::Instance(ucx_mgr_)) {
    spdlog::warn("Client::Initialize: UCX manager unavailable, "
                 "PutObjectRdma will fail");
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

bool Client::PutObject(const PutObjectRequest& request,
                      ConstBufferView buffer,
                      TransferOutcome& out) const {
  if (!initialized_) {
    spdlog::error("PutObject: {}", kNotInitializedMsg);
    return false;
  }

  const auto max_put = options_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    spdlog::warn("PutObject: bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
                 "use multipart upload",
                 request.bucket, request.key, buffer.size, max_put);
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  return RetryIfRetryable(RetryPolicy{}, [&]() -> bool {
    if (std::chrono::steady_clock::now() >= deadline) {
      spdlog::warn("PutObject: bucket={}/{} retry deadline exceeded", request.bucket, request.key);
      return false;
    }
    return PutOnce(options_, *proxy_, gds_mgr_, request, buffer, out);
  });
}

// RDMA(UCX)链路的单次尝试：AcquireDescriptor → RdmaPut。
// 与 gds 的 PutOnce 完全独立，不复用。
[[nodiscard]] bool RdmaPutOnce(const ClientOptions& options,
                               const ProxyRpc& proxy,
                               UcxMemoryManager* ucx_mgr,
                               const PutObjectRequest& request,
                               ConstBufferView buffer,
                               TransferOutcome& out) {
  assert(ucx_mgr != nullptr);
  const std::string request_id = MakeRequestId();
  const auto timeout = ResolveTimeout(options, request);

  const bool trace = options.latency_trace;
  auto t0 = clk::now();

  UcxMemoryManager::Descriptor desc;
  if (!ucx_mgr->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    return false;
  }
  auto t_desc = clk::now();

  GdsPutResult result;
  if (!proxy.RdmaPut(request, request_id, timeout, desc.remote_addr, desc.rkey,
                     desc.client_ucx_addr, buffer.size, result)) {
    return false;
  }
  auto t_put = clk::now();

  out.bytes_transferred = buffer.size;
  out.request_id        = request_id;
  out.etag              = result.etag;
  out.crc32c            = result.crc32c;

  // 端到端 CRC32C 校验（可选）：host buffer 直接算本地 CRC（无需 D2H 拷贝，
  // 这是 rdma 链路相比 gds 的一个便利），与 backend 回传的 crc32c 比对。
  if (options.verify_crc32c) {
    const std::uint32_t local = Crc32c(
        std::span<const std::byte>(static_cast<const std::byte*>(buffer.data),
                                   buffer.size));
    const std::uint32_t remote = result.crc32c;
    if (local == remote) {
      spdlog::info("RdmaPut (req={}): crc32c MATCH local={:08x} remote={:08x} "
                   "bucket={}/{} bytes={}",
                   request_id, local, remote, request.bucket, request.key,
                   buffer.size);
    } else {
      spdlog::error("RdmaPut (req={}): crc32c MISMATCH local={:08x} remote={:08x} "
                    "bucket={}/{} bytes={}",
                    request_id, local, remote, request.bucket, request.key,
                    buffer.size);
      return false;
    }
  }

  if (trace) {
    const auto ms = [](clk::time_point a, clk::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    spdlog::info("RdmaPut trace (req={}): desc={:.3f}ms "
                 "put={:.3f}ms total={:.3f}ms bytes={}",
                 request_id, ms(t0, t_desc),
                 ms(t_desc, t_put), ms(t0, t_put), buffer.size);
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

bool Client::PutObjectRdma(const PutObjectRequest& request,
                           ConstBufferView buffer,
                           TransferOutcome& out) const {
  if (!initialized_) {
    spdlog::error("PutObjectRdma: {}", kNotInitializedMsg);
    return false;
  }
  if (ucx_mgr_ == nullptr) {
    spdlog::error("PutObjectRdma: UCX manager not initialized");
    return false;
  }

  const auto max_put = options_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    spdlog::warn("PutObjectRdma: bucket={}/{} body size {} exceeds put_single_max_bytes {}",
                 request.bucket, request.key, buffer.size, max_put);
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  return RetryIfRetryable(RetryPolicy{}, [&]() -> bool {
    if (std::chrono::steady_clock::now() >= deadline) {
      spdlog::warn("PutObjectRdma: bucket={}/{} retry deadline exceeded", request.bucket, request.key);
      return false;
    }
    return RdmaPutOnce(options_, *proxy_, ucx_mgr_, request, buffer, out);
  });
}

}  // namespace us3_turbo::client
