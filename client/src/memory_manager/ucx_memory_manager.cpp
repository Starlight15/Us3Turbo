#include "client/src/memory_manager/ucx_memory_manager.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace us3_turbo::client {

namespace {

// 默认 client UCX listener 绑定地址。IP 必须是 mlx5 上可达的;
// 端口 0 由系统分配,ucp_listener_query 取回实际端口。
constexpr char kDefaultBindIp[] = "192.168.1.198";

}  // namespace

// listener conn_handler:接受 backend 连接(ucp_ep_create(CONN_REQUEST)),否则连接挂起。
// client 侧不持有 ep,依赖进程退出回收。TODO(v2): 维护 ep 池,按完成回调 close。
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

// ---- 分阶段 init:构造函数逐阶段调用,失败按反向顺序 cleanup。 ----

bool UcxMemoryManager::InitContext() {
  ucp_config_t* config = nullptr;
  if (ucp_config_read(nullptr, nullptr, &config) != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_config_read failed");
    return false;
  }

  ucp_params_t ctx_params{};
  ctx_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  ctx_params.features = UCP_FEATURE_RMA;

  ucs_status_t st = ucp_init(&ctx_params, config, &context_);
  ucp_config_release(config);

  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_init failed: {}", ucs_status_string(st));
    context_ = nullptr;
    return false;
  }

  return true;
}

bool UcxMemoryManager::InitWorker() {
  assert(context_ != nullptr);  // 前置条件:InitContext 成功

  // UCS_THREAD_MODE_MULTI:client 单例被多 worker 线程共享,且 listener
  // conn_handler 在 UCX 内部线程触发。MULTI 保证跨线程访问 worker 安全。
  ucp_worker_params_t wparams{};
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wparams.thread_mode = UCS_THREAD_MODE_MULTI;

  ucs_status_t st = ucp_worker_create(context_, &wparams, &worker_);
  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_worker_create failed: {}",
                  ucs_status_string(st));
    worker_ = nullptr;
    return false;
  }

  return true;
}

bool UcxMemoryManager::InitListener() {
  assert(worker_ != nullptr);  // 前置条件:InitWorker 成功

  // 端口 0 让系统分配,再 query 取回实际绑定地址(随 Descriptor 透传)。
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;  // 系统分配
  if (inet_pton(AF_INET, kDefaultBindIp, &addr.sin_addr) != 1) {
    spdlog::error("UcxMemoryManager: invalid bind ip {}", kDefaultBindIp);
    return false;
  }

  ucp_listener_params_t lparams{};
  lparams.field_mask =
      UCP_LISTENER_PARAM_FIELD_SOCK_ADDR | UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lparams.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);
  lparams.sockaddr.addrlen = sizeof(addr);
  lparams.conn_handler.cb = &UcxMemoryManager::ConnCallback;
  lparams.conn_handler.arg = this;

  ucs_status_t st = ucp_listener_create(worker_, &lparams, &listener_);
  if (st != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_listener_create failed: {}",
                  ucs_status_string(st));
    listener_ = nullptr;
    return false;
  }

  // 查询实际绑定地址，随 Descriptor 透传给 backend。
  ucp_listener_attr_t lattr{};
  lattr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
  if (ucp_listener_query(listener_, &lattr) != UCS_OK) {
    spdlog::error("UcxMemoryManager: ucp_listener_query failed");
    return false;
  }
  char host[NI_MAXHOST] = {};
  char serv[NI_MAXSERV] = {};
  if (getnameinfo(reinterpret_cast<struct sockaddr*>(&lattr.sockaddr),
                  sizeof(lattr.sockaddr), host, sizeof(host), serv, sizeof(serv),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    spdlog::error("UcxMemoryManager: getnameinfo failed");
    return false;
  }
  listen_addr_ = std::string(host) + ":" + std::string(serv);

  spdlog::info("UcxMemoryManager: listener at {}", listen_addr_);
  return true;
}

void UcxMemoryManager::StartProgressThread() {
  assert(worker_ != nullptr);  // 前置条件

  // 后台 progress 线程驱动 listener conn_handler(见头注释)。不请求
  // UCP_FEATURE_WAKEUP,用 spin + idle sleep:无事件时短暂 sleep 让出 CPU。
  progress_thread_ = std::thread([this]() {
    constexpr auto kIdleSleep = std::chrono::microseconds(50);
    while (!stop_.load(std::memory_order_acquire)) {
      if (ucp_worker_progress(worker_) == 0) {
        std::this_thread::sleep_for(kIdleSleep);
      }
    }
  });
}

void UcxMemoryManager::CleanupContext() {
  if (context_ != nullptr) {
    ucp_cleanup(context_);
    context_ = nullptr;
  }
}

void UcxMemoryManager::CleanupWorker() {
  if (worker_ != nullptr) {
    ucp_worker_destroy(worker_);
    worker_ = nullptr;
  }
}

void UcxMemoryManager::CleanupListener() {
  if (listener_ != nullptr) {
    ucp_listener_destroy(listener_);
    listener_ = nullptr;
  }
}

// ---- 构造/析构:逐阶段 init,失败按反向顺序回滚。 ----

UcxMemoryManager::UcxMemoryManager() {
  if (!InitContext()) {
    return;
  }
  if (!InitWorker()) {
    CleanupContext();
    return;
  }
  if (!InitListener()) {
    CleanupWorker();
    CleanupContext();
    return;
  }

  StartProgressThread();
  started_ = true;
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
    ClearRegistered();
  }
  CleanupListener();
  CleanupWorker();
  CleanupContext();
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

bool UcxMemoryManager::DoRegister(void* ptr, std::size_t size, ucp_mem_h& out) {
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
  out = memh;
  return true;
}

void UcxMemoryManager::DoUnregister(void* /*ptr*/, ucp_mem_h& handle) {
  if (handle != nullptr) {
    ucp_mem_unmap(context_, handle);
    handle = nullptr;
  }
}

bool UcxMemoryManager::AcquireDescriptor(const void* ptr, std::size_t size,
                                         Descriptor& out) {
  if (ptr == nullptr || size == 0U) {
    spdlog::warn("UcxMemoryManager::AcquireDescriptor: requires non-null ptr and positive size");
    return false;
  }
  void* mut_ptr = const_cast<void*>(ptr);

  std::scoped_lock lk(mu_);
  // 幂等注册:已注册直接复用,未注册则 DoRegister。
  ucp_mem_h* p_memh = FindLocked(mut_ptr);
  if (p_memh == nullptr) {
    if (!BufferRegistry::RegisterBuffer(mut_ptr, size)) return false;
    p_memh = FindLocked(mut_ptr);
    if (p_memh == nullptr) return false;  // 不应发生
  }
  ucp_mem_h memh = *p_memh;
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
