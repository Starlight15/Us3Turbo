#include "client/src/gds_transport/gds_memory_manager.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <cufile.h>
#include <cuobjclient.h>

#include "client/src/common/errors.h"

namespace us3_turbo::client {
namespace {

// cuObj PUT/GET 回调在 token-direct 路径下永远不会触发；SDK 构造 client
// 时要求 ops 表两个字段非空，给一个返回 -1 的桩。
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}
ssize_t StubPut(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}

}  // namespace

/* ===== GdsMemoryManager::Impl ===== */

struct GdsMemoryManager::Impl {
  // cuObjClient 构造需要 ops 引用，必须持久化存储
  CUObjOps_t                   ops{};
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

/* ===== GdsMemoryManager::Token ===== */

GdsMemoryManager::Token::Token(Token&& other) noexcept
    : client_(other.client_), tok_(other.tok_) {
  other.client_ = nullptr;
  other.tok_    = nullptr;
}

GdsMemoryManager::Token& GdsMemoryManager::Token::operator=(Token&& other) noexcept {
  if (this != &other) {
    Reset();
    client_       = other.client_;
    tok_          = other.tok_;
    other.client_ = nullptr;
    other.tok_    = nullptr;
  }
  return *this;
}

GdsMemoryManager::Token::~Token() { Reset(); }

void GdsMemoryManager::Token::Reset() noexcept {
  if (tok_ != nullptr && client_ != nullptr) {
    client_->cuMemObjPutRDMAToken(tok_);
  }
  client_ = nullptr;
  tok_    = nullptr;
}

std::string_view GdsMemoryManager::Token::str() const noexcept {
  if (tok_ == nullptr) return {};
  return std::string_view(tok_);
}

/* ===== GdsMemoryManager ===== */

GdsMemoryManager::GdsMemoryManager() : impl_(std::make_unique<Impl>()) {
  connected_ = impl_->client && impl_->client->isConnected();
}

GdsMemoryManager::~GdsMemoryManager() {
  // 进程退出 best-effort 反注册，避免下次 cudaFree 时 nvidia-fs 死锁
  if (!registered_.empty()) {
    std::cerr << "[GdsMemoryManager] WARNING: " << registered_.size()
              << " device buffer(s) not unregistered before shutdown; "
                 "calling cuMemObjPutDescriptor as best-effort"
              << std::endl;
    for (auto& [ptr, _] : registered_) {
      if (impl_->client) {
        impl_->client->cuMemObjPutDescriptor(ptr);
      }
    }
    registered_.clear();
  }
}

Result<GdsMemoryManager*> GdsMemoryManager::Instance() {
  static Result<GdsMemoryManager*> holder = []() -> Result<GdsMemoryManager*> {
    static GdsMemoryManager mgr;
    if (!mgr.connected_) {
      return Result<GdsMemoryManager*>::Failure(MakeTransportFailure(
          "cuObjClient 未连接到可用的 RDMA 服务", DataFlow::GPUDirect, "", true));
    }
    return Result<GdsMemoryManager*>::Success(&mgr);
  }();
  return holder;
}

Result<bool> GdsMemoryManager::RegisterBuffer(void* ptr, std::size_t size) {
  if (ptr == nullptr || size == 0U) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "RegisterBuffer requires non-null ptr and positive size"));
  }
  std::scoped_lock lk(registration_mu_);
  return RegisterBufferUnderLock(ptr, size);
}

Result<bool> GdsMemoryManager::UnregisterBuffer(void* ptr) {
  if (ptr == nullptr) {
    return Result<bool>::Failure(
        MakeInvalidArgument("UnregisterBuffer requires non-null ptr"));
  }
  std::scoped_lock lk(registration_mu_);
  auto it = registered_.find(ptr);
  if (it == registered_.end()) {
    return Result<bool>::Success(true);  // 双反注册 / 从未注册过，幂等
  }
  const auto rc = impl_->client->cuMemObjPutDescriptor(ptr);
  registered_.erase(it);
  if (rc != CU_OBJ_SUCCESS) {
    return Result<bool>::Failure(MakeTransportFailure(
        "cuMemObjPutDescriptor 释放失败", DataFlow::GPUDirect, "", true));
  }
  return Result<bool>::Success(true);
}

Result<GdsMemoryManager::Token>
GdsMemoryManager::AcquireToken(const void* ptr, std::size_t size,
                               std::size_t offset, OperationType op) {
  if (ptr == nullptr || size == 0U) {
    return Result<Token>::Failure(MakeInvalidArgument(
        "AcquireToken requires non-null ptr and positive size"));
  }
  // 内部 const_cast 一次：cuMemObjGetRDMAToken 签名是 CUdeviceptr(本质
  // uint64_t)，CUDA 驱动不修改 buffer 内容
  void* mut_ptr = const_cast<void*>(ptr);

  // 数据路径不持锁：注册路径已在 RegisterBuffer 完成；这里只读 descriptor
  // 表确认存在，未存在则 lazy register(持锁)
  bool need_register = false;
  {
    std::scoped_lock lk(registration_mu_);
    need_register = registered_.find(mut_ptr) == registered_.end();
  }
  if (need_register) {
    std::scoped_lock lk(registration_mu_);
    auto reg = RegisterBufferUnderLock(mut_ptr, size + offset);
    if (!reg.success()) return Result<Token>::Failure(reg.error());
  }

  const auto cu_op = op == OperationType::kGet ? CUOBJ_GET : CUOBJ_PUT;
  char* tok = nullptr;
  const auto rc = impl_->client->cuMemObjGetRDMAToken(mut_ptr, size, offset, cu_op, &tok);
  if (rc != CU_OBJ_SUCCESS || tok == nullptr) {
    return Result<Token>::Failure(MakeTransportFailure(
        "cuMemObjGetRDMAToken 获取失败", DataFlow::GPUDirect, "", true));
  }
  return Result<Token>::Success(Token(impl_->client.get(), tok));
}

Result<bool> GdsMemoryManager::RegisterBufferUnderLock(void* ptr, std::size_t size) {
  // 调用方持 registration_mu_
  if (registered_.find(ptr) != registered_.end()) {
    return Result<bool>::Success(true);  // 已注册，size 已在 SDK 内部记下
  }
  const auto rc = impl_->client->cuMemObjGetDescriptor(ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    return Result<bool>::Failure(MakeTransportFailure(
        "cuMemObjGetDescriptor 注册失败", DataFlow::GPUDirect, "", true));
  }
  registered_.emplace(ptr, size);
  return Result<bool>::Success(true);
}

}  // namespace us3_turbo::client
