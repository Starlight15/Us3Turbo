// rdma_memory_manager.cpp — RDMA 链路的 client 端内存管理器实现。

#include "client/src/memory_manager/rdma_memory_manager.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "us3_turbo/common/logger.h"

#include <netdb.h>

namespace us3_turbo::client {

namespace {

// client RDMA CM listener 绑定地址，端口 0 由系统分配。
// bind IP 解析优先级（仅首次 Instance 调用生效）：
//   ClientOptions::rdma_bind_ip > 环境变量 US3_TURBO_RDMA_BIND_IP。
// 两者都未指定时 Instance 记 info 返回，不再用内置 fallback（避免跨环境绑到错误网卡）。
// g_bind_ip 初值 "0.0.0.0" 表示"尚未解析"。
// 注意：最终 IP 会被写入 RDMA token 供 backend 反向连接，必须是可路由的 RDMA 网卡 IP
//（不能用 0.0.0.0，否则 backend 无法 RDMA-CONNECT）。
std::string g_bind_ip = "0.0.0.0";

// 单 Accept 超时（ms）：后台线程用短超时走 select 轮询，stop_ 时可快速退出。
constexpr int kAcceptTimeoutMs = 500;

// 最大缓存已接受 QP 数：连接池化后 QP 持久复用，128 覆盖 conc=16×4qps 场景。
constexpr std::size_t kMaxCachedQps = 128;

}  // namespace

// ---- 分阶段 init：失败按反向顺序 cleanup。 ----

bool RdmaMemoryManager::InitListener() {
  listener_ = RdmaQp::CreateListener(g_bind_ip.c_str(), 0);
  if (listener_ == nullptr) {
    LOG_SYS_ERROR("RdmaQp CreateListener({}:0) failed", g_bind_ip);
    return false;
  }

  ibv_context* ctx = listener_->device();
  if (ctx == nullptr) {
    LOG_SYS_ERROR("listener device() returned null");
    return false;
  }

  pd_ = ibv_alloc_pd(ctx);
  if (pd_ == nullptr) {
    LOG_SYS_ERROR("ibv_alloc_pd failed");
    return false;
  }

  listen_ip_ = g_bind_ip;
  listen_port_ = listener_->listen_port();
  LOG_SYS_INFO("RDMA listener at {}:{}", listen_ip_, listen_port_);
  return true;
}

void RdmaMemoryManager::StartAcceptThread() {
  accept_thread_ = std::thread([this]() {
    AcceptLoop();
  });
}

void RdmaMemoryManager::AcceptLoop() {
  while (!stop_.load(std::memory_order_acquire)) {
    RdmaQp* qp = listener_->Accept(kAcceptTimeoutMs, pd_);
    if (qp == nullptr) {
      // 超时或错误，重试（stop_ 时自然退出）
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(qp_mu_);
      accepted_qps_.push_back(qp);
      // GC: 删除超出缓存的旧 QP
      while (accepted_qps_.size() > kMaxCachedQps) {
        delete accepted_qps_.front();
        accepted_qps_.erase(accepted_qps_.begin());
      }
    }
    LOG_SYS_INFO("RDMA QP accepted, cached={}", accepted_qps_.size());
  }
}

void RdmaMemoryManager::CleanupListener() {
  if (pd_ != nullptr) {
    ibv_dealloc_pd(pd_);
    pd_ = nullptr;
  }
  if (listener_ != nullptr) {
    delete listener_;
    listener_ = nullptr;
  }
}

// ---- 构造/析构 ----

RdmaMemoryManager::RdmaMemoryManager()
    : listener_(nullptr), pd_(nullptr), listen_port_(0) {
  if (!InitListener()) {
    return;
  }

  StartAcceptThread();
  started_ = true;
}

RdmaMemoryManager::~RdmaMemoryManager() {
  stop_.store(true, std::memory_order_release);
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lk(qp_mu_);
    for (auto* qp : accepted_qps_) {
      delete qp;
    }
    accepted_qps_.clear();
  }
  {
    std::unique_lock<std::shared_mutex> lk(mu_);
    for (auto& [ptr, mr] : registered_) {
      if (mr != nullptr) ibv_dereg_mr(mr);
    }
    ClearRegistered();
  }
  CleanupListener();
}

bool RdmaMemoryManager::Instance(RdmaMemoryManager*& out, const std::string& bind_ip) {
  // bind IP 解析优先级：显式参数 > 环境变量 US3_TURBO_RDMA_BIND_IP。
  // 两者都未指定时记 info 日志返回(未配置非故障)，不让 RDMA listener 用错误 IP 启动。
  // 仅首次调用生效（g_bind_ip 初值 "0.0.0.0" 表示未解析）；后续调用忽略。
  if (g_bind_ip == "0.0.0.0") {
    if (!bind_ip.empty()) {
      g_bind_ip = bind_ip;
    } else {
      const char* env = std::getenv("US3_TURBO_RDMA_BIND_IP");
      if (env != nullptr && env[0] != '\0') {
        g_bind_ip = env;
      } else {
        // 未配置 bind IP 属"未配置"而非故障，降为 info；RDMA 通路不可用，
        // client.cpp 会再以 warning 提示 kRdma 会失败。
        LOG_SYS_INFO(
            "RDMA bind IP 未指定，RDMA 通路不可用：请通过 --rdma-bind-ip 或环境变量 "
            "US3_TURBO_RDMA_BIND_IP 指定本机 RDMA 数据网卡 IP");
        return false;
      }
    }
  }
  static RdmaMemoryManager mgr;
  static bool init_ok = [&]() -> bool {
    if (mgr.started_) return true;
    LOG_SYS_ERROR("RDMA listener unavailable (rdma_bind_ip=\"{}\")", g_bind_ip);
    return false;
  }();
  if (!init_ok) return false;
  out = &mgr;
  return true;
}

bool RdmaMemoryManager::DoRegister(void* ptr, std::size_t size, ibv_mr*& out) {
  ibv_mr* mr = ibv_reg_mr(pd_, ptr, size, IBV_ACCESS_REMOTE_READ);
  if (mr == nullptr) {
    LOG_SYS_ERROR("ibv_reg_mr failed (ptr={} size={})", ptr, size);
    return false;
  }
  out = mr;
  return true;
}

void RdmaMemoryManager::DoUnregister(void* /*ptr*/, ibv_mr*& handle) {
  if (handle != nullptr) {
    ibv_dereg_mr(handle);
    handle = nullptr;
  }
}

bool RdmaMemoryManager::AcquireDescriptorImpl(const void* ptr, std::size_t size,
                                               int access_flags, const char* tag,
                                               Descriptor& out) {
  if (ptr == nullptr || size == 0U) {
    LOG_SYS_WARN("{} requires non-null ptr and positive size", tag);
    return false;
  }
  void* mut_ptr = const_cast<void*>(ptr);

  // Fast path: shared_lock 允许并发 cache-hit 无阻塞。
  ibv_mr* mr{};
  bool cache_hit = false;
  {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = registered_.find(mut_ptr);
    cache_hit = (it != registered_.end());
    if (cache_hit) mr = it->second;
  }

  // Slow path: cache-miss 需要 exclusive lock 做 ibv_reg_mr (仅首次串行)。
  if (!cache_hit) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = registered_.find(mut_ptr);
    if (it != registered_.end()) {
      // Double-check: 可能被另一个线程在 shared→unique 间隙注册了。
      cache_hit = true;
      mr = it->second;
    } else {
      mr = ibv_reg_mr(pd_, mut_ptr, size, access_flags);
      if (mr == nullptr) {
        LOG_SYS_ERROR("{} ibv_reg_mr failed ptr={} size={} access={:#x}", tag, mut_ptr, size,
                      access_flags);
        return false;
      }
      registered_.emplace(mut_ptr, mr);
      cache_hit = true;
    }
  }

  // Token 编码（锁外）。
  out.token = EncodeToken(listen_ip_.c_str(), listen_port_,
                          mr->rkey, reinterpret_cast<std::uint64_t>(mut_ptr),
                          size);
  return true;
}

bool RdmaMemoryManager::AcquireDescriptor(const void* ptr, std::size_t size,
                                           Descriptor& out) {
  return AcquireDescriptorImpl(ptr, size, IBV_ACCESS_REMOTE_READ,
                               "AcquireDescriptor", out);
}

bool RdmaMemoryManager::AcquireDescriptorForWrite(const void* ptr, std::size_t size,
                                                   Descriptor& out) {
  return AcquireDescriptorImpl(
      ptr, size,
      IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE,
      "AcquireDescriptorForWrite", out);
}

}  // namespace us3_turbo::client
