#include "us3_turbo/client/client.h"

#include <cassert>
#include <chrono>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include <spdlog/spdlog.h>

#include "client/src/contracts/request_builder.h"
#include "client/src/contracts/requests.h"
#include "client/src/control/meta_rpc.h"
#include "client/src/data/chunk_rpc.h"
#include "client/src/data/crc32c.h"
#include "client/src/gds_transport/gds_memory_manager.h"

namespace us3_turbo::client {
namespace {

using clk = std::chrono::steady_clock;

constexpr char kNotInitializedMsg[] =
    "Client is not initialized. Call Client::Initialize first.";

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
                          MetaRpc& meta,
                          const ChunkRpc& chunk,
                          GdsMemoryManager* gds_mgr,
                          const PutObjectRequest& request,
                          ConstBufferView buffer,
                          TransferOutcome& out) {
  assert(gds_mgr != nullptr);
  auto attempt = MakePutAttempt(options, request);

  const bool trace = options.latency_trace;
  auto t0 = clk::now();

  SessionGrant grant;
  if (!meta.OpenSession(attempt, grant)) return false;
  auto t_open = clk::now();

  GdsMemoryManager::Token token;
  if (!gds_mgr->AcquireToken(buffer.data, buffer.size, 0, token)) {
    meta.AbortSession(attempt.session_id, attempt.timeout);
    return false;
  }
  auto t_token = clk::now();

  GdsPutResult result;
  if (!chunk.Put(attempt, grant, token.str(), buffer.size, result)) {
    meta.AbortSession(attempt.session_id, attempt.timeout);
    return false;
  }
  auto t_put = clk::now();

  out = MakeTransferOutcome(attempt, result, buffer);

  // 端到端 CRC32C 一致性校验(可选):把 device buffer 拷回 host 计算本地 CRC32C,
  // 与 backend 在 GdsChunkResponse.crc32c 回传的值比对。不一致视为失败(返回 false,
  // 上层 RetryIfRetryable 会重试),并记录 MISMATCH 日志。
  if (options.verify_crc32c) {
    std::vector<std::byte> host(buffer.size);
    if (cudaError_t e = cudaMemcpy(host.data(), buffer.data, buffer.size,
                                   cudaMemcpyDeviceToHost); e != cudaSuccess) {
      spdlog::error("GdsPut (req={}): verify_crc32c D2H copy failed: {}",
                    attempt.request_id, cudaGetErrorString(e));
      meta.AbortSession(attempt.session_id, attempt.timeout);
      return false;
    }
    const std::uint32_t local = Crc32c(std::span<const std::byte>(host.data(), host.size()));
    const std::uint32_t remote = result.crc32c;
    if (local == remote) {
      spdlog::info("GdsPut (req={}): crc32c MATCH local={:08x} remote={:08x} "
                   "bucket={}/{} bytes={}",
                   attempt.request_id, local, remote, attempt.bucket, attempt.key,
                   buffer.size);
    } else {
      spdlog::error("GdsPut (req={}): crc32c MISMATCH local={:08x} remote={:08x} "
                    "bucket={}/{} bytes={}",
                    attempt.request_id, local, remote, attempt.bucket, attempt.key,
                    buffer.size);
      meta.AbortSession(attempt.session_id, attempt.timeout);
      return false;
    }
  }

  if (trace) {
    const auto ms = [](clk::time_point a, clk::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    spdlog::info("GdsPut trace (req={}): open={:.3f}ms token={:.3f}ms "
                 "put={:.3f}ms total={:.3f}ms bytes={}",
                 attempt.request_id, ms(t0, t_open), ms(t_open, t_token),
                 ms(t_token, t_put), ms(t0, t_put), buffer.size);
  }

  return true;
}

}  // namespace

Client::Client(ClientOptions options) : options_(std::move(options)) {}
Client::~Client() = default;

bool Client::Initialize() {
  if (initialized_) return true;

  meta_ = std::make_unique<MetaRpc>(options_.endpoint, options_.default_timeout);
  if (!meta_->ok()) {
    spdlog::error("Initialize: control-plane channel({}) init failed: {}",
                  options_.endpoint, meta_->init_error());
    meta_.reset();
    return false;
  }

  chunk_ = std::make_unique<ChunkRpc>(options_.gds_data_endpoint, options_.default_timeout);
  if (!chunk_->ok()) {
    spdlog::error("Initialize: data-plane channel({}) init failed: {}",
                  options_.gds_data_endpoint, chunk_->init_error());
    chunk_.reset();
    meta_.reset();
    return false;
  }

  if (!GdsMemoryManager::Instance(gds_mgr_)) {
    chunk_.reset();
    meta_.reset();
    return false;
  }

  initialized_ = true;
  return true;
}

void Client::Shutdown() {
  chunk_.reset();
  meta_.reset();
  gds_mgr_ = nullptr;
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
    return PutOnce(options_, *meta_, *chunk_, gds_mgr_, request, buffer, out);
  });
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
