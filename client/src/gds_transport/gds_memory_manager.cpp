#include "client/src/gds_transport/gds_memory_manager.h"

#include <cstddef>
#include <string>
#include <utility>

#include <cufile.h>
#include <cuobjclient.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::client {
namespace {
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) { return -1; }
ssize_t StubPut(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*) { return -1; }
}  // namespace

struct GdsMemoryManager::Impl {
  CUObjOps_t                   ops{};
  std::unique_ptr<cuObjClient> client;
  Impl() {
    ops.get = &StubGet;
    ops.put = &StubPut;
    try { client = std::make_unique<cuObjClient>(ops, CUOBJ_PROTO_RDMA_DC_V1); }
    catch (...) { client.reset(); }
  }
};

GdsMemoryManager::Token::Token(Token&& o) noexcept : client_(o.client_), tok_(o.tok_) {
  o.client_ = nullptr; o.tok_ = nullptr;
}
GdsMemoryManager::Token& GdsMemoryManager::Token::operator=(Token&& o) noexcept {
  if (this != &o) { Reset(); client_ = o.client_; tok_ = o.tok_; o.client_ = nullptr; o.tok_ = nullptr; }
  return *this;
}
GdsMemoryManager::Token::~Token() { Reset(); }
void GdsMemoryManager::Token::Reset() noexcept {
  if (tok_ && client_) client_->cuMemObjPutRDMAToken(tok_);
  client_ = nullptr; tok_ = nullptr;
}
std::string_view GdsMemoryManager::Token::str() const noexcept {
  return tok_ ? std::string_view(tok_) : std::string_view{};
}

GdsMemoryManager::GdsMemoryManager() : impl_(std::make_unique<Impl>()) {
  connected_ = impl_->client && impl_->client->isConnected();
}
GdsMemoryManager::~GdsMemoryManager() {
  if (!registered_.empty()) {
    spdlog::warn("[GdsMemoryManager] {} buffer(s) not unregistered before shutdown", registered_.size());
    for (auto& [ptr, _] : registered_)
      if (impl_->client) impl_->client->cuMemObjPutDescriptor(ptr);
    registered_.clear();
  }
}

bool GdsMemoryManager::Instance(GdsMemoryManager*& out) {
  static GdsMemoryManager mgr;
  static bool init_ok = [&]() -> bool {
    if (mgr.connected_) return true;
    spdlog::error("GdsMemoryManager: cuObjClient not connected to RDMA service");
    return false;
  }();
  if (!init_ok) return false;
  out = &mgr;
  return true;
}

bool GdsMemoryManager::RegisterBuffer(void* ptr, std::size_t size) {
  if (!ptr || size == 0U) {
    spdlog::warn("RegisterBuffer: requires non-null ptr and positive size (ptr={} size={})",
                 ptr, size);
    return false;
  }
  std::scoped_lock lk(registration_mu_);
  return RegisterBufferUnderLock(ptr, size);
}

bool GdsMemoryManager::UnregisterBuffer(void* ptr) {
  if (!ptr) {
    spdlog::warn("UnregisterBuffer: requires non-null ptr");
    return false;
  }
  std::scoped_lock lk(registration_mu_);
  auto it = registered_.find(ptr);
  if (it == registered_.end()) return true;
  const auto rc = impl_->client->cuMemObjPutDescriptor(ptr);
  registered_.erase(it);
  if (rc != CU_OBJ_SUCCESS) {
    spdlog::error("UnregisterBuffer: cuMemObjPutDescriptor failed (ptr={} rc={})", ptr, rc);
    return false;
  }
  return true;
}

bool GdsMemoryManager::AcquireToken(const void* ptr, std::size_t size,
                                   std::size_t offset, Token& out) {
  if (!ptr || size == 0U) {
    spdlog::warn("AcquireToken: requires non-null ptr and positive size (ptr={} size={})",
                 ptr, size);
    return false;
  }

  void* mut_ptr = const_cast<void*>(ptr);

  // 单次加锁，在锁保护下完成注册检查（RegisterBufferUnderLock 幂等）。
  // 消除旧实现的双重检查锁定竞态：原实现解锁→再加锁之间有窗口期，且
  // 第二次加锁后未复查 registered_，多线程下可能重复注册。
  {
    std::scoped_lock lk(registration_mu_);
    if (!RegisterBufferUnderLock(mut_ptr, size + offset)) {
      return false;
    }
  }
  // cuMemObjGetRDMAToken 是外部库调用，可能耗时较长，在锁外执行以提高
  // 并发性：RegisterBufferUnderLock 已确保 impl_->client 有效且 buffer 已注册。
  char* tok = nullptr;
  const auto rc = impl_->client->cuMemObjGetRDMAToken(mut_ptr, size, offset, CUOBJ_PUT, &tok);
  if (rc != CU_OBJ_SUCCESS || !tok) {
    spdlog::error("AcquireToken: cuMemObjGetRDMAToken failed (ptr={} size={} offset={} rc={})",
                  ptr, size, offset, rc);
    return false;
  }
  // 打印 backend 用以 RDMA-READ 的 token（形如 "hexaddr:rkey"），
  // 便于跨机调试时核对 client 显存地址 + remote key 是否落在对端可达的 fabric 上。
  spdlog::info("AcquireToken: ptr={} size={} offset={} rdma_token={}",
               ptr, size, offset, tok);
  out = Token(impl_->client.get(), tok);
  return true;
}

bool GdsMemoryManager::RegisterBufferUnderLock(void* ptr, std::size_t size) {
  if (registered_.find(ptr) != registered_.end()) return true;
  const auto rc = impl_->client->cuMemObjGetDescriptor(ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    spdlog::error("RegisterBuffer: cuMemObjGetDescriptor failed (ptr={} size={} rc={})",
                  ptr, size, rc);
    return false;
  }
  registered_.emplace(ptr, size);
  return true;
}

}  // namespace us3_turbo::client
