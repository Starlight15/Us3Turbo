#include "client/src/memory_manager/gds_memory_manager.h"

#include <chrono>
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

bool GdsMemoryManager::AcquireToken(const void* ptr, std::size_t size, std::size_t offset,
                                    Token& out, cuObjOpType_t op) {
  if (!ptr || size == 0U) {
    LOG_SYS_WARN("requires non-null ptr and positive size (ptr={} size={})", ptr, size);
    return false;
  }

  void* mut_ptr = const_cast<void*>(ptr);

  // [诊断插桩] 分阶段计时,定位并发瓶颈:buffer_id 查询 / 等锁 / 持锁
  // (查表+可能的 DoRegister) / 锁外(GetRDMAToken)。验证完毕后可整块删除。
  using diag_clk = std::chrono::steady_clock;
  const auto t0 = diag_clk::now();

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
  const auto t1 = diag_clk::now();  // t1-t0 = buffer_id 查询耗时

  bool cache_hit = false;
  diag_clk::time_point t2, t3;
  {
    std::lock_guard<std::mutex> lk(mu_);
    t2 = diag_clk::now();  // 锁已到手:t2-t1 = 等锁耗时
    const std::size_t needed = size + offset;
    auto it = registered_.find(mut_ptr);
    if (it != registered_.end()) {
      const bool reused = it->second.buffer_id != buf_id;
      const bool too_small = it->second.size < needed;
      cache_hit = !(reused || too_small);
      if (reused || too_small) {
        // clang-format off
        LOG_SYS_INFO("re-register ptr={} reason={} old_sz={} new_sz={} old_id={} new_id={}", mut_ptr, reused ? "reused" : "grew", it->second.size, needed, it->second.buffer_id, buf_id);
        // clang-format on
        DoUnregister(mut_ptr, it->second);
        registered_.erase(it);
        GdsRegEntry entry{};
        entry.buffer_id = buf_id;
        if (!DoRegister(mut_ptr, needed, entry)) return false;
        registered_.emplace(mut_ptr, entry);
      }
    } else {
      GdsRegEntry entry{};
      entry.buffer_id = buf_id;
      if (!DoRegister(mut_ptr, needed, entry)) return false;
      registered_.emplace(mut_ptr, entry);
    }
    t3 = diag_clk::now();  // t3-t2 = 持锁期间耗时(查表,miss/reused 时含 DoRegister)
  }

  // GetRDMAToken 在锁外执行以允许并发。
  char* tok = nullptr;
  const auto ret = impl_->client->cuMemObjGetRDMAToken(mut_ptr, size, offset, op, &tok);
  if (ret != CU_OBJ_SUCCESS || !tok) {
    LOG_SYS_ERROR("cuMemObjGetRDMAToken failed (ptr={} size={} offset={} op={} rc={})", ptr, size,
                  offset, static_cast<int>(op), ret);
    return false;
  }
  const auto t4 = diag_clk::now();  // t4-t3 = GetRDMAToken 耗时

  const auto diag_us = [](diag_clk::time_point a, diag_clk::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
  };
  LOG_SYS_INFO(
      "ptr={} size={} offset={} op={} rdma_token={} "
      "PHASE hit={} bufid_us={:.1f} lock_wait_us={:.1f} inlock_us={:.1f} token_us={:.1f}",
      ptr, size, offset, static_cast<int>(op), tok, cache_hit, diag_us(t0, t1), diag_us(t1, t2),
      diag_us(t2, t3), diag_us(t3, t4));
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
