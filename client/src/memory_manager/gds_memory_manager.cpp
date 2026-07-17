#include "client/src/memory_manager/gds_memory_manager.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "us3_turbo/common/logger.h"

#include <cufile.h>
#include <cuobjclient.h>

namespace us3_turbo::client {
namespace {
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}
ssize_t StubPut(const void*, const char*, size_t, loff_t,
                const cufileRDMAInfo_t*) {
  return -1;
}

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

GdsMemoryManager::Token::Token(Token&& o) noexcept
    : client_(o.client_), tok_(o.tok_) {
  o.client_ = nullptr;
  o.tok_ = nullptr;
}
GdsMemoryManager::Token& GdsMemoryManager::Token::operator=(
    Token&& o) noexcept {
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
    LOG_SYS_DEBUG("{} buffer(s) in cache at shutdown (懒注册常驻)",
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
    LOG_SYS_WARN("requires non-null ptr and positive size (ptr={} size={})",
                 ptr, size);
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
                                    std::size_t offset, Token& out,
                                    cuObjOpType_t operation) {
  if (!ptr || size == 0U) {
    LOG_SYS_WARN("requires non-null ptr and positive size (ptr={} size={})",
                 ptr, size);
    return false;
  }

  void* mut_ptr = const_cast<void*>(ptr);

  // 查询当前地址上"住户"的进程内唯一分配 ID。CUDA 保证:同一进程内,
  // 不同分配的 buffer_id 绝不重复,free 后也不会被后来者捡走
  // (见 cuda.h CU_POINTER_ATTRIBUTE_BUFFER_ID 文档)。
  // 复用检测完全依赖该属性,查询失败直接拒绝,不做 size-only 降级——
  // 旧驱动/不支持环境下 GDS 视为不可用,由调用方决定是否走非 GDS 路径。
  unsigned long long cur_buffer_id = 0;
  const CUresult pr =
      cuPointerGetAttribute(&cur_buffer_id, CU_POINTER_ATTRIBUTE_BUFFER_ID,
                            reinterpret_cast<CUdeviceptr>(mut_ptr));
  if (pr != CUDA_SUCCESS) {
    LOG_SYS_ERROR(
        "cuPointerGetAttribute(BUFFER_ID) failed (ptr={} rc={}), cannot "
        "verify address-reuse identity, refusing to register",
        ptr, static_cast<int>(pr));
    return false;  // 身份查不到就不能判断复用,直接失败,不降级。
  }

  // 单次加锁完成幂等注册检查,DoRegister 失败则回滚占位。
  // 重新 pin 的条件:地址被复用(buffer_id 变了) || 旧注册范围不够。
  {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t needed = size + offset;
    auto it = registered_.find(mut_ptr);
    if (it != registered_.end()) {
      const bool addr_reused = it->second.buffer_id != cur_buffer_id;
      const bool too_small = it->second.size < needed;
      if (addr_reused || too_small) {
        LOG_SYS_INFO(
            "re-register ptr={} reason={} old_size={} new_size={} old_bufid={} "
            "new_bufid={}",
            mut_ptr,
            addr_reused ? "buffer reused at same address" : "size grew",
            it->second.size, needed, it->second.buffer_id, cur_buffer_id);
        DoUnregister(mut_ptr, it->second);
        registered_.erase(it);  // 先清旧条目,避免"已 unregister 但 key 还在"
        GdsRegEntry entry{};
        entry.buffer_id = cur_buffer_id;  // 提前填充
        if (!DoRegister(mut_ptr, needed, entry)) return false;
        registered_.emplace(mut_ptr, entry);
      }
      // 身份未变且范围足够:无需再 pin。
    } else {
      GdsRegEntry entry{};
      entry.buffer_id = cur_buffer_id;  // 提前填充
      if (!DoRegister(mut_ptr, needed, entry)) return false;
      registered_.emplace(mut_ptr, entry);
    }
  }
  // cuMemObjGetRDMAToken 可能耗时较长,锁外执行以提高并发性。
  char* tok = nullptr;
  const auto rc = impl_->client->cuMemObjGetRDMAToken(mut_ptr, size, offset,
                                                      operation, &tok);
  if (rc != CU_OBJ_SUCCESS || !tok) {
    LOG_SYS_ERROR(
        "cuMemObjGetRDMAToken failed (ptr={} size={} offset={} op={} rc={})",
        ptr, size, offset, static_cast<int>(operation), rc);
    return false;
  }
  LOG_SYS_INFO("ptr={} size={} offset={} op={} rdma_token={}", ptr, size,
               offset, static_cast<int>(operation), tok);
  out = Token(impl_->client.get(), tok);
  return true;
}

bool GdsMemoryManager::DoRegister(void* ptr, std::size_t size,
                                  GdsRegEntry& out) {
  const auto rc = impl_->client->cuMemObjGetDescriptor(ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    LOG_SYS_ERROR("cuMemObjGetDescriptor failed (ptr={} size={} rc={})", ptr,
                  size, rc);
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
