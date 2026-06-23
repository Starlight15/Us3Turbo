#pragma once

#include <cstddef>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "client/src/gds_transport/cuobj_library.h"
#include "us3_turbo/client/result.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief 一段 cuObject RDMA token 的 RAII 持有者。
 *
 * token 是 cuObj SDK 分配的 C 字符串,发到 backend、RPC 响应回来后必须释放。
 * 析构调 put_rdma_token;move 后原实例 valid()==false。client-new 自有定义。
 */
class GdsRdmaToken {
 public:
  GdsRdmaToken() = default;
  GdsRdmaToken(GdsRdmaToken&& other) noexcept;
  GdsRdmaToken& operator=(GdsRdmaToken&& other) noexcept;
  GdsRdmaToken(const GdsRdmaToken&) = delete;
  GdsRdmaToken& operator=(const GdsRdmaToken&) = delete;
  ~GdsRdmaToken();

  [[nodiscard]] std::string_view str() const noexcept;
  [[nodiscard]] bool             valid() const noexcept { return tok_ != nullptr; }

 private:
  friend class GdsMemoryRegistry;
  GdsRdmaToken(const CuObjApi* api, void* client, char* tok) noexcept
      : api_(api), client_(client), tok_(tok) {}
  void Reset() noexcept;

  const CuObjApi* api_{nullptr};
  void*           client_{nullptr};
  char*           tok_{nullptr};
};

/**
 * @brief GDS GPU buffer 注册器 / RDMA token 颁发器。
 *
 * 状态落在进程级单例 CuObjState 上(cuObj descriptor 表 process-wide,且
 * 只起一个 cuObj client)。RegisterBuffer 把 device buffer 一次性 pin 入
 * BAR1;AcquireToken 在 PUT 数据面颁发 RDMA token。未注册的 ptr 会 lazy
 * register(建议提前 RegisterBuffer 避免数据面 syscall 抖动)。
 *
 * 用户契约:
 *   - RegisterBuffer / UnregisterBuffer idempotent。
 *   - cudaFree(ptr) 之前必须 UnregisterBuffer(ptr)(nvidia-fs 死锁风险)。
 */
class GdsMemoryRegistry {
 public:
  GdsMemoryRegistry()  = default;
  ~GdsMemoryRegistry() = default;
  GdsMemoryRegistry(const GdsMemoryRegistry&)            = delete;
  GdsMemoryRegistry& operator=(const GdsMemoryRegistry&) = delete;

  /** 显式注册 device buffer,idempotent。 */
  [[nodiscard]] Result<bool> RegisterBuffer(void* ptr, std::size_t size);
  /** 显式注销,必须在 cudaFree(ptr) 之前调;idempotent。 */
  [[nodiscard]] Result<bool> UnregisterBuffer(void* ptr);
  /**
   * 取一段 (ptr, offset, size, op) 的 RDMA token;RAII 析构释放。
   * PUT 路径 const 重载内部 const_cast 一次(签名是 CUdeviceptr)。
   */
  [[nodiscard]] Result<GdsRdmaToken>
    AcquireToken(const void* ptr, std::size_t size, std::size_t offset, OperationType op);

  /**
   * @brief 探测 libcuobjclient 是否可加载。
   *
   * 与数据面 lazy 路径同源(共享 CuObjLibrary 单例),失败返回带原因的
   * kTransportError。供 Client::Initialize 做 GDS 通路一次性早探测,避免
   * 首次 PutObject 才崩在半路。成功后 lazy 路径命中缓存,零额外开销。
   */
  [[nodiscard]] static Result<bool> ProbeLibrary();
};

}  // namespace us3_turbo::client
