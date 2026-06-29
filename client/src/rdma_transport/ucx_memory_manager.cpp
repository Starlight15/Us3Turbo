#include "client/src/rdma_transport/ucx_memory_manager.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace us3_turbo::client {

namespace {

// 默认 client UCX listener 绑定地址。可通过 UCX_TLS/IB 等 env 覆盖，
// 但 IP 必须是 mlx5 上可达的。默认用 192.168.1.198 + 0（系统分配端口，
// 由 ucp_listener_query 取回实际端口）。
constexpr char kDefaultBindIp[] = "192.168.1.198";

}  // namespace

// listener conn_handler：client 收到 backend 的连接请求。第一版只是
// 接受连接（不主动在 client 侧建 ep——backend 会用这个连接完成握手后
// 发起 get_nbx）。我们调用 ucp_ep_create(CONN_REQUEST) 让 UCX 接受它，
// 否则连接挂起。ep 在 client 侧第一版不持有/管理：依赖进程退出回收。
// TODO(v2): 维护 ep 池，按完成回调 close。
void UcxMemoryManager::ConnCallback(ucp_conn_request_h req, void* arg) {
  auto* self = static_cast<UcxMemoryManager*>(arg);
  if (self == nullptr || self->worker_ == nullptr) {
    if (self != nullptr && self->listener_ != nullptr) {
      ucp_listener_reject(self->listener_, req);
    }
    return;
  }
  std::scoped_lock lk(self->mu_);
  ucp_ep_params_t ep_params{};
  ep_params.field_mask =
      UCP_EP_PARAM_FIELD_CONN_REQUEST | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
  ep_params.conn_request = req;
  ep_params.err_mode = UCP_ERR_HANDLING_MODE_NONE;
  ucp_ep_h ep = nullptr;
  ucs_status_t st = ucp_ep_create(self->worker_, &ep_params, &ep);
  if (st != UCS_OK) {
    spdlog::warn("UcxMemoryManager: conn cb ucp_ep_create failed: {}",
                 ucs_status_string(st));
  }
}

UcxMemoryManager::UcxMemoryManager() {
  ucp_params_t ctx_params{};
  ctx_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  ctx_params.features = UCP_FEATURE_RMA;
  ucp_config_t* config = nullptr;
  if (ucp_config_read(nullptr, nullptr, &config) != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_config_read failed");
    return;
  }
  ucs_status_t st = ucp_init(&ctx_params, config, &context_);
  ucp_config_release(config);
  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_init failed: {}", ucs_status_string(st));
    return;
  }

  // UCS_THREAD_MODE_MULTI：client 单例被 bench 多 worker 线程共享，且
  // listener conn_handler 在 UCX 内部线程触发、与 AcquireDescriptor 调用
  // 线程不同。MULTI 保证跨线程访问 worker 安全。
  ucp_worker_params_t wparams{};
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wparams.thread_mode = UCS_THREAD_MODE_MULTI;
  st = ucp_worker_create(context_, &wparams, &worker_);
  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_worker_create failed: {}",
                  ucs_status_string(st));
    ucp_cleanup(context_);
    context_ = nullptr;
    return;
  }

  // listener 绑定 kDefaultBindIp，端口 0 让系统分配，再 query 取回。
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;  // 系统分配
  if (inet_pton(AF_INET, kDefaultBindIp, &addr.sin_addr) != 1) {
    spdlog::error("UcxMemoryManager: invalid bind ip {}", kDefaultBindIp);
    return;
  }
  ucp_listener_params_t lparams{};
  lparams.field_mask =
      UCP_LISTENER_PARAM_FIELD_SOCK_ADDR | UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lparams.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);
  lparams.sockaddr.addrlen = sizeof(addr);
  lparams.conn_handler.cb = &UcxMemoryManager::ConnCallback;
  lparams.conn_handler.arg = this;
  st = ucp_listener_create(worker_, &lparams, &listener_);
  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_listener_create failed: {}",
                  ucs_status_string(st));
    return;
  }

  ucp_listener_attr_t lattr{};
  lattr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
  if (ucp_listener_query(listener_, &lattr) != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_listener_query failed");
    return;
  }
  char host[NI_MAXHOST] = {};
  char serv[NI_MAXSERV] = {};
  if (getnameinfo(reinterpret_cast<struct sockaddr*>(&lattr.sockaddr),
                  sizeof(lattr.sockaddr), host, sizeof(host), serv, sizeof(serv),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    spdlog::error("UcxMemoryManager: getnameinfo failed");
    return;
  }
  listen_addr_ = std::string(host) + ":" + std::string(serv);

  started_ = true;
  spdlog::info("UcxMemoryManager: listener at {}", listen_addr_);

  // 起后台 progress 线程驱动 listener 的 conn_handler（见头注释）。
  // 不请求 UCP_FEATURE_WAKEUP（避免 eventfd/signal 路径），用 spin + idle
  // sleep：progress 返回 0（无事件）时短暂 sleep 让出 CPU，有事件时立即
  // 再 progress。停止时 stop_ 标志在下个迭代生效，无需 signal。
  progress_thread_ = std::thread([this]() {
    constexpr auto kIdleSleep = std::chrono::microseconds(50);
    while (!stop_.load(std::memory_order_acquire)) {
      if (ucp_worker_progress(worker_) == 0) {
        std::this_thread::sleep_for(kIdleSleep);
      }
    }
  });
}

UcxMemoryManager::~UcxMemoryManager() {
  stop_.store(true, std::memory_order_release);
  if (progress_thread_.joinable()) {
    progress_thread_.join();
  }
  {
    std::scoped_lock lk(mu_);
    for (auto& [ptr, memh] : registered_) {
      if (memh != nullptr) ucp_mem_unmap(context_, memh);
    }
    registered_.clear();
  }
  if (listener_ != nullptr) ucp_listener_destroy(listener_);
  if (worker_ != nullptr) ucp_worker_destroy(worker_);
  if (context_ != nullptr) ucp_cleanup(context_);
}

bool UcxMemoryManager::Instance(UcxMemoryManager*& out) {
  static UcxMemoryManager mgr;
  static bool init_ok = [&]() -> bool {
    if (mgr.started_) return true;
    spdlog::error("UcxMemoryManager: not started (UCX listener unavailable)");
    return false;
  }();
  if (!init_ok) return false;
  out = &mgr;
  return true;
}

bool UcxMemoryManager::RegisterBufferUnderLock(void* ptr, std::size_t size) {
  if (registered_.find(ptr) != registered_.end()) return true;
  ucp_mem_map_params_t mparams{};
  mparams.field_mask =
      UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  mparams.address = ptr;
  mparams.length = size;
  ucp_mem_h memh = nullptr;
  ucs_status_t st = ucp_mem_map(context_, &mparams, &memh);
  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_mem_map failed (ptr={} size={} {})",
                  ptr, size, ucs_status_string(st));
    return false;
  }
  registered_.emplace(ptr, memh);
  return true;
}

bool UcxMemoryManager::AcquireDescriptor(const void* ptr, std::size_t size,
                                         Descriptor& out) {
  if (ptr == nullptr || size == 0U) {
    spdlog::warn("UcxMemoryManager::AcquireDescriptor: requires non-null ptr and positive size");
    return false;
  }
  void* mut_ptr = const_cast<void*>(ptr);

  std::scoped_lock lk(mu_);
  if (registered_.find(mut_ptr) == registered_.end()) {
    if (!RegisterBufferUnderLock(mut_ptr, size)) return false;
  }
  ucp_mem_h memh = registered_[mut_ptr];
  void* rkey_buf = nullptr;
  size_t rkey_size = 0;
  ucs_status_t st = ucp_rkey_pack(context_, memh, &rkey_buf, &rkey_size);
  if (st != UCS_OK || rkey_buf == nullptr) {
    spdlog::error("UcxMemoryManager: ucp_rkey_pack failed: {}",
                  ucs_status_string(st));
    return false;
  }
  out.remote_addr = reinterpret_cast<std::uint64_t>(mut_ptr);
  out.rkey.assign(static_cast<const char*>(rkey_buf), rkey_size);
  out.client_ucx_addr = listen_addr_;
  ucp_rkey_buffer_release(rkey_buf);

  spdlog::info("UcxMemoryManager::AcquireDescriptor: ptr={} size={} "
               "remote_addr=0x{:x} rkey_bytes={} ucx_addr={}",
               ptr, size, out.remote_addr, out.rkey.size(), out.client_ucx_addr);
  return true;
}

}  // namespace us3_turbo::client
