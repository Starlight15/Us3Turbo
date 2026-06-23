#include "client/src/gds_transport/gds_memory_registry.h"

#include <cstddef>
#include <iostream>
#include <new>
#include <string>
#include <utility>

#include <cufile.h>
#include <cuobjclient.h>

#include "client/src/common/errors.h"

namespace us3_turbo::client {
namespace {

/* ===== 进程级单例:cuObj client + descriptor 缓存 ===== */

// cuObj PUT/GET 回调在 token-direct 路径下永远不会触发;SDK 构造 client
// 时要求 ops 表两个字段非空,给一个返 -1 的桩。
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}
ssize_t StubPut(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}

class CuObjState {
 public:
  // 返回进程唯一状态实例;首次访问触发库加载 + client 构造。
  static Result<CuObjState*> Get() {
    static Result<CuObjState*> holder = []() -> Result<CuObjState*> {
      auto lib = CuObjLibrary::Get();
      if (!lib.success()) {
        return Result<CuObjState*>::Failure(lib.error());
      }
      static CuObjState st(lib.value()->api());
      if (!st.connected_) {
        return Result<CuObjState*>::Failure(MakeTransportFailure(
            "cuObjClient 未连接到可用的 RDMA 服务", DataFlow::GPUDirect, "", true));
      }
      return Result<CuObjState*>::Success(&st);
    }();
    return holder;
  }

  [[nodiscard]] const CuObjApi& api() const { return api_; }
  [[nodiscard]] void*           client_raw() { return &client_storage_; }

  std::mutex                             registration_mu_;
  std::unordered_map<void*, std::size_t> registered_;

  ~CuObjState() {
    // 进程退出 best-effort 反注册,避免下次 cudaFree 时 nvidia-fs 死锁。
    if (!registered_.empty()) {
      std::cerr << "[gds_memory_registry] WARNING: " << registered_.size()
                << " device buffer(s) not unregistered before shutdown; "
                   "calling cuMemObjPutDescriptor as best-effort"
                << std::endl;
      for (auto& [ptr, _] : registered_) {
        api_.put_descriptor(&client_storage_, ptr);
      }
      registered_.clear();
    }
    if (constructed_) {
      api_.destructor(&client_storage_);
    }
  }

 private:
  explicit CuObjState(const CuObjApi& api) : api_(api) {
    ops_.get = &StubGet;
    ops_.put = &StubPut;
    api_.constructor(&client_storage_, ops_, CUOBJ_PROTO_RDMA_DC_V1);
    constructed_ = true;
    connected_   = api_.is_connected(&client_storage_);
    if (!connected_) {
      api_.destructor(&client_storage_);
      constructed_ = false;
    }
  }

  CuObjApi   api_;
  CUObjOps_t ops_{};
  bool       constructed_{false};
  bool       connected_{false};
  alignas(alignof(cuObjClient)) std::byte
      client_storage_[std::max(sizeof(cuObjClient), std::size_t{4096})];
};

[[nodiscard]] Result<bool> RegisterUnderLock(CuObjState& st, void* ptr,
                                             std::size_t size) {
  // 调用方持 st.registration_mu_。
  if (st.registered_.find(ptr) != st.registered_.end()) {
    return Result<bool>::Success(true);  // 已注册,size 已在 SDK 内部记下
  }
  const auto rc = st.api().get_descriptor(st.client_raw(), ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    return Result<bool>::Failure(MakeTransportFailure(
        "cuMemObjGetDescriptor 注册失败", DataFlow::GPUDirect, "", true));
  }
  st.registered_.emplace(ptr, size);
  return Result<bool>::Success(true);
}

}  // namespace

/* ===== GdsRdmaToken ===== */

GdsRdmaToken::GdsRdmaToken(GdsRdmaToken&& other) noexcept
    : api_(other.api_), client_(other.client_), tok_(other.tok_) {
  other.api_    = nullptr;
  other.client_ = nullptr;
  other.tok_    = nullptr;
}

GdsRdmaToken& GdsRdmaToken::operator=(GdsRdmaToken&& other) noexcept {
  if (this != &other) {
    Reset();
    api_          = other.api_;
    client_       = other.client_;
    tok_          = other.tok_;
    other.api_    = nullptr;
    other.client_ = nullptr;
    other.tok_    = nullptr;
  }
  return *this;
}

GdsRdmaToken::~GdsRdmaToken() { Reset(); }

void GdsRdmaToken::Reset() noexcept {
  if (tok_ != nullptr && api_ != nullptr) {
    api_->put_rdma_token(client_, tok_);
  }
  api_    = nullptr;
  client_ = nullptr;
  tok_    = nullptr;
}

std::string_view GdsRdmaToken::str() const noexcept {
  if (tok_ == nullptr) return {};
  return std::string_view(tok_);
}

/* ===== GdsMemoryRegistry ===== */

Result<bool> GdsMemoryRegistry::RegisterBuffer(void* ptr, std::size_t size) {
  if (ptr == nullptr || size == 0U) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "RegisterDeviceBuffer requires non-null ptr and positive size"));
  }
  auto state = CuObjState::Get();
  if (!state.success()) return Result<bool>::Failure(state.error());
  std::scoped_lock lk(state.value()->registration_mu_);
  return RegisterUnderLock(*state.value(), ptr, size);
}

Result<bool> GdsMemoryRegistry::UnregisterBuffer(void* ptr) {
  if (ptr == nullptr) {
    return Result<bool>::Failure(
        MakeInvalidArgument("UnregisterDeviceBuffer requires non-null ptr"));
  }
  auto state = CuObjState::Get();
  if (!state.success()) return Result<bool>::Failure(state.error());
  auto& st = *state.value();
  std::scoped_lock lk(st.registration_mu_);
  auto it = st.registered_.find(ptr);
  if (it == st.registered_.end()) {
    return Result<bool>::Success(true);  // 双反注册 / 从未注册过,idempotent
  }
  const auto rc = st.api().put_descriptor(st.client_raw(), ptr);
  st.registered_.erase(it);
  if (rc != CU_OBJ_SUCCESS) {
    return Result<bool>::Failure(MakeTransportFailure(
        "cuMemObjPutDescriptor 释放失败", DataFlow::GPUDirect, "", true));
  }
  return Result<bool>::Success(true);
}

Result<GdsRdmaToken>
GdsMemoryRegistry::AcquireToken(const void* ptr, std::size_t size,
                                std::size_t offset, OperationType op) {
  if (ptr == nullptr || size == 0U) {
    return Result<GdsRdmaToken>::Failure(MakeInvalidArgument(
        "AcquireToken requires non-null ptr and positive size"));
  }
  // 内部 const_cast 一次:cuMemObjGetRDMAToken 签名是 CUdeviceptr(本质
  // uint64_t),CUDA 驱动不修改 buffer 内容。
  void* mut_ptr = const_cast<void*>(ptr);
  auto state = CuObjState::Get();
  if (!state.success()) return Result<GdsRdmaToken>::Failure(state.error());
  auto& st = *state.value();

  // 数据路径不持锁:注册路径已在 RegisterBuffer 完成;这里只读 descriptor
  // 表确认存在,未存在则 lazy register(持锁)。
  bool need_register = false;
  {
    std::scoped_lock lk(st.registration_mu_);
    need_register = st.registered_.find(mut_ptr) == st.registered_.end();
  }
  if (need_register) {
    std::scoped_lock lk(st.registration_mu_);
    auto reg = RegisterUnderLock(st, mut_ptr, size + offset);
    if (!reg.success()) return Result<GdsRdmaToken>::Failure(reg.error());
  }

  const auto cu_op = op == OperationType::kGet ? CUOBJ_GET : CUOBJ_PUT;
  char* tok = nullptr;
  const auto rc =
      st.api().get_rdma_token(st.client_raw(), mut_ptr, size, offset, cu_op, &tok);
  if (rc != CU_OBJ_SUCCESS || tok == nullptr) {
    return Result<GdsRdmaToken>::Failure(MakeTransportFailure(
        "cuMemObjGetRDMAToken 获取失败", DataFlow::GPUDirect, "", true));
  }
  return Result<GdsRdmaToken>::Success(
      GdsRdmaToken(&st.api(), st.client_raw(), tok));
}

Result<bool> GdsMemoryRegistry::ProbeLibrary() {
  // 与数据面 lazy 路径同源:CuObjLibrary::Get() 进程单例首次加载即 dlopen
  // libcuobjclient 候选 + 解析符号。成功后 lazy 路径命中缓存,零额外开销。
  auto lib = CuObjLibrary::Get();
  if (!lib.success()) return Result<bool>::Failure(lib.error());
  return Result<bool>::Success(true);
}

}  // namespace us3_turbo::client
