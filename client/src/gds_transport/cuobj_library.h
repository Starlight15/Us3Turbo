#pragma once

#include <cstddef>

#include <cufile.h>
#include <cuobjclient.h>

#include "us3_turbo/client/result.h"

namespace us3_turbo::client {

/**
 * @brief dlopen 解析得到的 libcuobjclient 函数指针表(client-new 自有定义)。
 *
 * 与旧 client 的 CuObjApi 等价,但归属 client-new 命名空间文件,不复用头。
 * 只保留 token-direct PUT 路径用到的符号:构造 / 析构 / isConnected /
 * get_descriptor / put_descriptor / get_rdma_token / put_rdma_token。
 */
struct CuObjApi {
  using Constructor = void (*)(void*, CUObjOps_t&, cuObjProto_t);
  using Destructor = void (*)(void*);
  using IsConnected = bool (*)(void*);
  using GetDescriptor = cuObjErr_t (*)(void*, void*, std::size_t);
  using PutDescriptor = cuObjErr_t (*)(void*, void*);
  using GetRDMAToken = cuObjErr_t (*)(void*, void*, std::size_t, std::size_t,
                                       cuObjOpType_t, char**);
  using PutRDMAToken = cuObjErr_t (*)(void*, char*);

  Constructor constructor{nullptr};
  Destructor destructor{nullptr};
  IsConnected is_connected{nullptr};
  GetDescriptor get_descriptor{nullptr};
  PutDescriptor put_descriptor{nullptr};
  GetRDMAToken get_rdma_token{nullptr};
  PutRDMAToken put_rdma_token{nullptr};
};

/**
 * @brief libcuobjclient 的动态加载封装,进程内全局单例。
 *
 * Get() 懒加载,失败返回错误。client-new 自有实现,不复用旧 client。
 */
class CuObjLibrary {
 public:
  CuObjLibrary(const CuObjLibrary&) = delete;
  CuObjLibrary& operator=(const CuObjLibrary&) = delete;
  CuObjLibrary(CuObjLibrary&& other) noexcept;
  CuObjLibrary& operator=(CuObjLibrary&& other) noexcept;
  ~CuObjLibrary();

  [[nodiscard]] const CuObjApi& api() const { return api_; }
  [[nodiscard]] static Result<const CuObjLibrary*> Get();

  // Internal: only Get() constructs via Load().
  CuObjLibrary(void* handle, CuObjApi api);

 private:
  CuObjLibrary() = default;
  void Reset();

  void*    handle_{nullptr};
  CuObjApi api_{};
};

}  // namespace us3_turbo::client
