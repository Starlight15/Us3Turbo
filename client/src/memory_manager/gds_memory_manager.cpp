#include "client/src/memory_manager/gds_memory_manager.h"

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
  // 懒注册常驻:注册表作进程级缓存,残留项是预期行为。
  if (RegisteredCount() != 0U) {
    spdlog::debug("[GdsMemoryManager] {} buffer(s) in cache at shutdown (懒注册常驻)",
                  RegisteredCount());
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [ptr, _] : registered_)
      if (impl_->client) impl_->client->cuMemObjPutDescriptor(ptr);
    ClearRegistered();
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

// null/size 校验 wrapper。
bool GdsMemoryManager::RegisterBuffer(void* ptr, std::size_t size) {
  if (!ptr || size == 0U) {
    spdlog::warn("RegisterBuffer: requires non-null ptr and positive size (ptr={} size={})",
                 ptr, size);
    return false;
  }
  return BufferRegistry::RegisterBuffer(ptr, size);
}

bool GdsMemoryManager::UnregisterBuffer(void* ptr) {
  if (!ptr) {
    spdlog::warn("UnregisterBuffer: requires non-null ptr");
    return false;
  }
  return BufferRegistry::UnregisterBuffer(ptr);
}

bool GdsMemoryManager::AcquireToken(const void* ptr, std::size_t size,
                                   std::size_t offset, Token& out) {
  if (!ptr || size == 0U) {
    spdlog::warn("AcquireToken: requires non-null ptr and positive size (ptr={} size={})",
                 ptr, size);
    return false;
  }

  void* mut_ptr = const_cast<void*>(ptr);

  // 单次加锁完成幂等注册检查,DoRegister 失败则回滚占位。
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (registered_.count(mut_ptr)) {
      // 已注册,无需再 pin。
    } else if (!DoRegister(mut_ptr, size + offset, registered_[mut_ptr])) {
      registered_.erase(mut_ptr);  // DoRegister 失败:回滚占位
      return false;
    }
  }
  // cuMemObjGetRDMAToken 可能耗时较长,锁外执行以提高并发性。
  char* tok = nullptr;
  const auto rc = impl_->client->cuMemObjGetRDMAToken(mut_ptr, size, offset, CUOBJ_PUT, &tok);
  if (rc != CU_OBJ_SUCCESS || !tok) {
    spdlog::error("AcquireToken: cuMemObjGetRDMAToken failed (ptr={} size={} offset={} rc={})",
                  ptr, size, offset, rc);
    return false;
  }
  // 打印 backend 用以 RDMA-READ 的 token("hexaddr:rkey"),跨机调试核对可达性。
  spdlog::info("AcquireToken: ptr={} size={} offset={} rdma_token={}",
               ptr, size, offset, tok);
  out = Token(impl_->client.get(), tok);
  return true;
}

bool GdsMemoryManager::DoRegister(void* ptr, std::size_t size, std::size_t& out) {
  const auto rc = impl_->client->cuMemObjGetDescriptor(ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    spdlog::error("RegisterBuffer: cuMemObjGetDescriptor failed (ptr={} size={} rc={})",
                  ptr, size, rc);
    return false;
  }
  out = size;  // GDS 句柄即 buffer size
  return true;
}

void GdsMemoryManager::DoUnregister(void* ptr, std::size_t& /*handle*/) {
  const auto rc = impl_->client->cuMemObjPutDescriptor(ptr);
  if (rc != CU_OBJ_SUCCESS) {
    spdlog::error("UnregisterBuffer: cuMemObjPutDescriptor failed (ptr={} rc={})", ptr, rc);
  }
}

}  // namespace us3_turbo::client
