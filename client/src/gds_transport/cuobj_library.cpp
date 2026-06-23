#include "client/src/gds_transport/cuobj_library.h"

#include <dlfcn.h>

#include <string>
#include <utility>

#include "client/src/common/errors.h"

namespace us3_turbo::client {
namespace {

constexpr const char* kCuObjLibraryCandidates[] = {
    "/usr/local/cuda/targets/x86_64-linux/lib/libcuobjclient.so.1",
    "/usr/local/cuda-13.2/targets/x86_64-linux/lib/libcuobjclient.so.1",
    "libcuobjclient.so.1",
    "libcuobjclient.so",
};

template <typename Symbol>
[[nodiscard]] Result<Symbol> ResolveSymbol(void* handle, const char* name) {
  dlerror();
  void* symbol = dlsym(handle, name);
  const char* error = dlerror();
  if (error != nullptr || symbol == nullptr) {
    return Result<Symbol>::Failure(MakeUnsupportedPath(
        DataFlow::GPUDirect, "Failed to resolve libcuobjclient symbol: " + std::string(name)));
  }
  return Result<Symbol>::Success(reinterpret_cast<Symbol>(symbol));
}

[[nodiscard]] Result<CuObjApi> ResolveApi(void* handle) {
  CuObjApi api;
  auto constructor = ResolveSymbol<CuObjApi::Constructor>(
      handle, "_ZN11cuObjClientC1ER10CUObjIOOps15cuObjProto_enum");
  if (!constructor.success()) return Result<CuObjApi>::Failure(constructor.error());

  auto destructor = ResolveSymbol<CuObjApi::Destructor>(handle, "_ZN11cuObjClientD1Ev");
  if (!destructor.success()) return Result<CuObjApi>::Failure(destructor.error());

  auto is_connected = ResolveSymbol<CuObjApi::IsConnected>(handle, "_ZN11cuObjClient11isConnectedEv");
  if (!is_connected.success()) return Result<CuObjApi>::Failure(is_connected.error());

  auto get_descriptor = ResolveSymbol<CuObjApi::GetDescriptor>(
      handle, "_ZN11cuObjClient21cuMemObjGetDescriptorEPvm");
  if (!get_descriptor.success()) return Result<CuObjApi>::Failure(get_descriptor.error());

  auto put_descriptor = ResolveSymbol<CuObjApi::PutDescriptor>(
      handle, "_ZN11cuObjClient21cuMemObjPutDescriptorEPv");
  if (!put_descriptor.success()) return Result<CuObjApi>::Failure(put_descriptor.error());

  auto get_rdma_token = ResolveSymbol<CuObjApi::GetRDMAToken>(
      handle, "_ZN11cuObjClient20cuMemObjGetRDMATokenEPvmm16cuObjOpType_enumPPc");
  if (!get_rdma_token.success()) return Result<CuObjApi>::Failure(get_rdma_token.error());

  auto put_rdma_token = ResolveSymbol<CuObjApi::PutRDMAToken>(
      handle, "_ZN11cuObjClient20cuMemObjPutRDMATokenEPc");
  if (!put_rdma_token.success()) return Result<CuObjApi>::Failure(put_rdma_token.error());

  api.constructor       = constructor.value();
  api.destructor        = destructor.value();
  api.is_connected      = is_connected.value();
  api.get_descriptor    = get_descriptor.value();
  api.put_descriptor    = put_descriptor.value();
  api.get_rdma_token    = get_rdma_token.value();
  api.put_rdma_token    = put_rdma_token.value();
  return Result<CuObjApi>::Success(api);
}

[[nodiscard]] Result<CuObjLibrary> Load() {
  for (const char* candidate : kCuObjLibraryCandidates) {
    void* handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      continue;
    }
    auto api = ResolveApi(handle);
    if (api.success()) {
      return Result<CuObjLibrary>::Success(CuObjLibrary(handle, api.value()));
    }
    dlclose(handle);
    return Result<CuObjLibrary>::Failure(api.error());
  }
  return Result<CuObjLibrary>::Failure(
      MakeUnsupportedPath(DataFlow::GPUDirect, "Failed to dynamically load libcuobjclient"));
}

}  // namespace

CuObjLibrary::CuObjLibrary(void* handle, CuObjApi api) : handle_(handle), api_(api) {}

CuObjLibrary::CuObjLibrary(CuObjLibrary&& other) noexcept
    : handle_(other.handle_), api_(other.api_) {
  other.handle_ = nullptr;
}

CuObjLibrary& CuObjLibrary::operator=(CuObjLibrary&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  handle_ = other.handle_;
  api_ = other.api_;
  other.handle_ = nullptr;
  return *this;
}

CuObjLibrary::~CuObjLibrary() { Reset(); }

void CuObjLibrary::Reset() {
  if (handle_ != nullptr) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

Result<const CuObjLibrary*> CuObjLibrary::Get() {
  static auto holder = Load();
  if (!holder.success()) {
    return Result<const CuObjLibrary*>::Failure(holder.error());
  }
  return Result<const CuObjLibrary*>::Success(&holder.value());
}

}  // namespace us3_turbo::client
