#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "us3_turbo/client/result.h"
#include "us3_turbo/client/types.h"

class cuObjClient;  // forward declaration from cuobjclient.h

namespace us3_turbo::client {

/**
 * @brief 进程唯一的 GDS 内存管理器 + RDMA token 颁发器。
 *
 * 职责：
 *   1. 初始化 cuObjClient（连接 RDMA 服务）
 *   2. 维护 GPU buffer 注册表（cuMemObjGetDescriptor/PutDescriptor）
 *   3. 颁发 RDMA token（cuMemObjGetRDMAToken）
 *
 * 线程安全：注册表受 mutex 保护，token 获取路径只读注册表（快速路径）。
 *
 * 用户契约：
 *   - RegisterBuffer/UnregisterBuffer 幂等
 *   - cudaFree(ptr) 前必须 UnregisterBuffer(ptr)（避免 nvidia-fs 死锁）
 *   - Token 必须在 RDMA 传输完成后析构（自动释放）
 */
class GdsMemoryManager {
 public:
  /**
   * @brief RDMA token 的 RAII 持有者。
   *
   * token 是 cuObj SDK 分配的 C 字符串，析构调 cuMemObjPutRDMAToken 释放。
   * move 后原实例 valid() == false。
   */
  class Token {
   public:
    Token() = default;
    Token(Token&& other) noexcept;
    Token& operator=(Token&& other) noexcept;
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    ~Token();

    [[nodiscard]] std::string_view str() const noexcept;
    [[nodiscard]] bool             valid() const noexcept { return tok_ != nullptr; }

   private:
    friend class GdsMemoryManager;
    Token(cuObjClient* client, char* tok) noexcept : client_(client), tok_(tok) {}
    void Reset() noexcept;

    cuObjClient* client_{nullptr};
    char*        tok_{nullptr};
  };

  /**
   * @brief 获取进程唯一实例。
   *
   * 首次调用触发 cuObjClient 构造 + isConnected() 检查。
   * 失败返回 kTransportError（RDMA 服务不可用）。
   */
  [[nodiscard]] static Result<GdsMemoryManager*> Instance();

  // ---- Buffer 注册管理 ----

  /** 显式注册 device buffer（pin 入 BAR1），幂等。 */
  [[nodiscard]] Result<bool> RegisterBuffer(void* ptr, std::size_t size);

  /** 显式注销，必须在 cudaFree(ptr) 前调用，幂等。 */
  [[nodiscard]] Result<bool> UnregisterBuffer(void* ptr);

  // ---- Token 获取 ----

  /**
   * @brief 获取 RDMA token（RAII 析构自动释放）。
   *
   * 未注册的 ptr 会 lazy register（建议提前 RegisterBuffer 避免数据面抖动）。
   * PUT 路径 const 重载内部 const_cast 一次（cuMemObjGetRDMAToken 签名限制）。
   */
  [[nodiscard]] Result<Token> AcquireToken(const void* ptr, std::size_t size,
                                           std::size_t offset, OperationType op);

  GdsMemoryManager(const GdsMemoryManager&)            = delete;
  GdsMemoryManager& operator=(const GdsMemoryManager&) = delete;

 private:
  GdsMemoryManager();
  ~GdsMemoryManager();

  [[nodiscard]] Result<bool> RegisterBufferUnderLock(void* ptr, std::size_t size);

  // Pimpl: 隐藏 cuObjClient 和 CUObjOps_t 的实现细节
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool                  connected_{false};

  std::mutex                             registration_mu_;
  std::unordered_map<void*, std::size_t> registered_;
};

}  // namespace us3_turbo::client
