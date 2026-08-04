#include "client/src/memory_manager/gds_memory_manager.h"

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "us3_turbo/common/logger.h"

#include <cufile.h>
#include <cuobjclient.h>

namespace us3_turbo::client {
namespace {
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) { return -1; }

ssize_t StubPut(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*) { return -1; }

}  // namespace

struct GdsMemoryManager::Impl {
  CUObjOps_t ops{};
  std::unique_ptr<cuObjClient> client;
  Impl() {
    ops.get = &StubGet;
    ops.put = &StubPut;
    try {
      client = std::make_unique<cuObjClient>(ops, CUOBJ_PROTO_RDMA_DC_V1);
    } catch (...) {
      client.reset();
    }
  }
};

GdsMemoryManager::Token::Token(Token&& o) noexcept : client_(o.client_), tok_(o.tok_) {
  o.client_ = nullptr;
  o.tok_ = nullptr;
}

GdsMemoryManager::Token& GdsMemoryManager::Token::operator=(Token&& o) noexcept {
  if (this != &o) {
    Reset();
    client_ = o.client_;
    tok_ = o.tok_;
    o.client_ = nullptr;
    o.tok_ = nullptr;
  }
  return *this;
}

GdsMemoryManager::Token::~Token() { Reset(); }

void GdsMemoryManager::Token::Reset() noexcept {
  if (tok_ && client_) client_->cuMemObjPutRDMAToken(tok_);
  client_ = nullptr;
  tok_ = nullptr;
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
    LOG_SYS_DEBUG("{} buffer(s) in cache at shutdown (懒注册常驻)", RegisteredCount());
    std::unique_lock<std::shared_mutex> lk(mu_);
    for (auto& [ptr, _] : registered_)
      if (impl_->client) impl_->client->cuMemObjPutDescriptor(ptr);
    ClearRegistered();
  }
}

bool GdsMemoryManager::Instance(GdsMemoryManager*& out) {
  static GdsMemoryManager mgr;
  static bool init_ok = [&]() -> bool {
    if (mgr.connected_) return true;
    LOG_SYS_ERROR("cuObjClient not connected to RDMA service");
    return false;
  }();
  if (!init_ok) return false;
  out = &mgr;
  return true;
}

// null/size 校验 wrapper。
bool GdsMemoryManager::RegisterBuffer(void* ptr, std::size_t size) {
  if (!ptr || size == 0U) {
    LOG_SYS_WARN("requires non-null ptr and positive size (ptr={} size={})", ptr, size);
    return false;
  }
  return BufferRegistry::RegisterBuffer(ptr, size);
}

bool GdsMemoryManager::UnregisterBuffer(void* ptr) {
  if (!ptr) {
    LOG_SYS_WARN("requires non-null ptr");
    return false;
  }
  return BufferRegistry::UnregisterBuffer(ptr);
}

bool GdsMemoryManager::AcquireToken(const void* ptr, std::size_t size,
                                    Token& out, cuObjOpType_t op) {
  if (!ptr || size == 0U) {
    LOG_SYS_WARN("requires non-null ptr and positive size (ptr={} size={})", ptr, size);
    return false;
  }

  void* mut_ptr = const_cast<void*>(ptr);

  // 查 buffer_id 检测地址复用:同进程内不同分配 ID 唯一,free 后不回收。
  // 查询失败直接拒绝,不做降级。
  unsigned long long buf_id = 0;
  const CUresult rc = cuPointerGetAttribute(&buf_id, CU_POINTER_ATTRIBUTE_BUFFER_ID,
                                            reinterpret_cast<CUdeviceptr>(mut_ptr));
  if (rc != CUDA_SUCCESS) {
    LOG_SYS_ERROR("cuPointerGetAttribute(BUFFER_ID) failed (ptr={} rc={})", ptr,
                  static_cast<int>(rc));
    return false;
  }

  bool cache_hit = false;

  // Fast path: shared_lock 允许并发 cache-hit 无阻塞。
  {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = registered_.find(mut_ptr);
    if (it != registered_.end()) {
      const bool reused = it->second.buffer_id != buf_id;
      const bool too_small = it->second.size < size;
      cache_hit = !(reused || too_small);
    }
  }

  // Slow path: cache-miss 或 re-register 需要 exclusive lock (仅首次/变更时串行)。
  if (!cache_hit) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = registered_.find(mut_ptr);
    if (it != registered_.end()) {
      // Double-check: 可能被另一个线程在 shared→unique 间隙注册了。
      const bool reused = it->second.buffer_id != buf_id;
      const bool too_small = it->second.size < size;
      if (!(reused || too_small)) {
        cache_hit = true;  // 竞态:另一线程已注册
      } else {
        // clang-format off
        LOG_SYS_INFO("re-register ptr={} reason={} old_sz={} new_sz={} old_id={} new_id={}", mut_ptr, reused ? "reused" : "grew", it->second.size, size, it->second.buffer_id, buf_id);
        // clang-format on
        DoUnregister(mut_ptr, it->second);
        registered_.erase(it);
        GdsRegEntry entry{};
        entry.buffer_id = buf_id;
        if (!DoRegister(mut_ptr, size, entry)) return false;
        registered_.emplace(mut_ptr, entry);
        cache_hit = true;
      }
    } else {
      GdsRegEntry entry{};
      entry.buffer_id = buf_id;
      if (!DoRegister(mut_ptr, size, entry)) return false;
      registered_.emplace(mut_ptr, entry);
      cache_hit = true;
    }
  }

  // GetRDMAToken 在锁外执行以允许并发。offset 恒为 0（全 region 注册）。
  char* tok = nullptr;
  const auto ret = impl_->client->cuMemObjGetRDMAToken(mut_ptr, size, 0, op, &tok);
  if (ret != CU_OBJ_SUCCESS || !tok) {
    LOG_SYS_ERROR("cuMemObjGetRDMAToken failed (ptr={} size={} op={} rc={})", ptr, size,
                  static_cast<int>(op), ret);
    return false;
  }
  out = Token(impl_->client.get(), tok);
  return true;
}

bool GdsMemoryManager::DoRegister(void* ptr, std::size_t size, GdsRegEntry& out) {
  const auto rc = impl_->client->cuMemObjGetDescriptor(ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    LOG_SYS_ERROR("cuMemObjGetDescriptor failed (ptr={} size={} rc={})", ptr, size, rc);
    return false;
  }
  out.size = size;  // GDS 句柄即 buffer size
  return true;
}

void GdsMemoryManager::DoUnregister(void* ptr, GdsRegEntry& /*handle*/) {
  const auto rc = impl_->client->cuMemObjPutDescriptor(ptr);
  if (rc != CU_OBJ_SUCCESS) {
    LOG_SYS_ERROR("cuMemObjPutDescriptor failed (ptr={} rc={})", ptr, rc);
  }
}

}  // namespace us3_turbo::client
