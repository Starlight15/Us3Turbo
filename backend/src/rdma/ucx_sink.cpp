#include "backend/src/rdma/ucx_sink.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "backend/src/rdma/ucx_resources.h"
#include "common/crc32c.h"

namespace us3_turbo::backend::rdma {

namespace {

using clk = std::chrono::steady_clock;
inline double MsSince(clk::time_point t) {
  return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

constexpr std::uint64_t kMaxChunkBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;  // 1 GiB

// 把 "ip:port" 解析成 sockaddr_in（UCX ep_create 的 SOCK_ADDR 需要）。
// 仅支持 IPv4（与 134 的 mlx5 以太网 IP 一致）。失败返回 false。
[[nodiscard]] bool ParseAddr(const std::string& ip_port, sockaddr_in& out) {
  const auto colon = ip_port.rfind(':');
  if (colon == std::string::npos) return false;
  const std::string ip = ip_port.substr(0, colon);
  const std::string port = ip_port.substr(colon + 1);
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  char* end = nullptr;
  const long p = std::strtol(port.c_str(), &end, 10);
  if (end == port.c_str() || p <= 0 || p > 65535) return false;
  out.sin_port = htons(static_cast<std::uint16_t>(p));
  return inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

// 等待一个 UCX 请求完成（get/send/recv/close 通用）。
// 返回 UCS_OK 表示成功；否则返回错误 status。req 为 nullptr 视为内联完成。
ucs_status_t WaitReq(ucp_worker_h worker, void* req) {
  if (req == nullptr) return UCS_OK;
  if (UCS_PTR_IS_ERR(req)) {
    const ucs_status_t st = UCS_PTR_STATUS(req);
    ucp_request_free(req);
    return st;
  }
  while (ucp_request_check_status(req) == UCS_INPROGRESS) {
    ucp_worker_progress(worker);
  }
  const ucs_status_t st = ucp_request_check_status(req);
  ucp_request_free(req);
  return st;
}

}  // namespace

UcxSink::UcxSink(bool compute_crc32c) : compute_crc32c_(compute_crc32c) {}

UcxSink::~UcxSink() { Stop(); }

bool UcxSink::Start() {
  ucp_params_t ctx_params{};
  ctx_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  // RMA 给 ucp_get_nbx；本 sink 不用 tag（descriptor 经 brpc 透传，不走 UCX tag）。
  ctx_params.features = UCP_FEATURE_RMA;

  ucp_config_t* config = nullptr;
  if (ucp_config_read(nullptr, nullptr, &config) != UCS_OK) {
    spdlog::error("backend.ucx: ucp_config_read failed");
    return false;
  }
  ucs_status_t st = ucp_init(&ctx_params, config, &context_);
  ucp_config_release(config);
  if (st != UCS_OK) {
    spdlog::error("backend.ucx: ucp_init failed: {}", ucs_status_string(st));
    return false;
  }

  // UCS_THREAD_MODE_MULTI：brpc 用 bthread 并发调用 RdmaPut，不同 bthread
  // 会从不同 OS 线程访问同一 worker（progress / ep_create / get_nbx）。
  // SINGLE 模式下只有创建 worker 的线程可访问，跨线程访问会让 conn_request
  // 处理被拒（实测 "Operation rejected by remote peer"）。MULTI 让任意 bthread
  // 安全访问。worker_mu_ 仍保留以串行化关键段、避免竞态。
  ucp_worker_params_t wparams{};
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wparams.thread_mode = UCS_THREAD_MODE_MULTI;
  st = ucp_worker_create(context_, &wparams, &worker_);
  if (st != UCS_OK) {
    spdlog::error("backend.ucx: ucp_worker_create failed: {}", ucs_status_string(st));
    ucp_cleanup(context_);
    context_ = nullptr;
    return false;
  }

  spdlog::info("backend.ucx: context + worker ready");
  return true;
}

void UcxSink::Stop() {
  if (worker_ != nullptr) {
    ucp_worker_destroy(worker_);
    worker_ = nullptr;
  }
  if (context_ != nullptr) {
    ucp_cleanup(context_);
    context_ = nullptr;
  }
}

bool UcxSink::available() const {
  return context_ != nullptr && worker_ != nullptr;
}

DiscardOutcome UcxSink::ReceiveAndDiscard(const std::string& object_id,
                                          const std::string& client_ucx_addr,
                                          std::uint64_t remote_addr,
                                          const std::string& rkey,
                                          std::uint64_t length) {
  DiscardOutcome outcome;

  if (length > kMaxChunkBytes) {
    outcome.error = "RDMA PUT chunk exceeds 1 GiB limit";
    return outcome;
  }
  if (length == 0U) {
    outcome.ok = true;
    return outcome;
  }
  if (rkey.empty()) {
    outcome.error = "empty rkey";
    return outcome;
  }

  std::scoped_lock lock(worker_mu_);  // 单 worker 非线程安全，串行化。

  // ---- 1. dial client 建 ep（SOCK_ADDR + CLIENT_SERVER flag）----
  sockaddr_in addr{};
  if (!ParseAddr(client_ucx_addr, addr)) {
    outcome.error = "invalid client_ucx_addr: " + client_ucx_addr;
    return outcome;
  }
  ucp_ep_params_t ep_params{};
  ep_params.field_mask =
      UCP_EP_PARAM_FIELD_SOCK_ADDR | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
      UCP_EP_PARAM_FIELD_FLAGS;
  ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  ep_params.err_mode = UCP_ERR_HANDLING_MODE_NONE;
  ep_params.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);
  ep_params.sockaddr.addrlen = sizeof(addr);
  ucp_ep_h ep = nullptr;
  ucs_status_t st = ucp_ep_create(worker_, &ep_params, &ep);
  if (st != UCS_OK) {
    outcome.error = std::string("ucp_ep_create failed: ") + ucs_status_string(st);
    return outcome;
  }
  EpGuard ep_guard(worker_, ep);

  // ---- 2. unpack rkey（在 dial 出的 ep 上）----
  ucp_rkey_h rkey_h = nullptr;
  st = ucp_ep_rkey_unpack(ep, rkey.data(), &rkey_h);
  if (st != UCS_OK) {
    outcome.error = std::string("ucp_ep_rkey_unpack failed: ") + ucs_status_string(st);
    return outcome;
  }
  RkeyGuard rkey_guard(rkey_h);

  // ---- 3. malloc 本地 buffer + ucp_get_nbx 拉取 ----
  HostBuffer buf(static_cast<std::size_t>(length));
  if (!buf.ok()) {
    outcome.error = "malloc local buffer failed";
    return outcome;
  }

  const auto t0 = clk::now();
  ucp_request_param_t get_param{};
  void* get_req = ucp_get_nbx(ep, buf.data(), static_cast<size_t>(length),
                              remote_addr, rkey_h, &get_param);
  st = WaitReq(worker_, get_req);
  const double rdma_ms = MsSince(t0);
  if (st != UCS_OK) {
    spdlog::info("backend.ucxput object={} length={} rdma_ms={:.3f} status={}",
                 object_id, length, rdma_ms, ucs_status_string(st));
    outcome.error = std::string("ucp_get_nbx failed: ") + ucs_status_string(st);
    return outcome;
  }

  // ---- 4. CRC32C（可选）----
  double crc_ms = 0.0;
  if (compute_crc32c_) {
    const auto c0 = clk::now();
    outcome.crc32c = us3_turbo::gateway::common::Crc32c(
        std::span<const std::byte>(static_cast<const std::byte*>(buf.data()),
                                   static_cast<std::size_t>(length)));
    crc_ms = MsSince(c0);
  }
  spdlog::info("backend.ucxput object={} length={} rdma_ms={:.3f} "
               "crc_ms={:.3f} crc32c={:x}",
               object_id, length, rdma_ms, crc_ms, outcome.crc32c);
  // 【丢弃】不写盘。
  outcome.ok = true;
  outcome.bytes_transferred = length;
  return outcome;
}

}  // namespace us3_turbo::backend::rdma
