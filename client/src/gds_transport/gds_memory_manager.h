#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "client/src/transport/buffer_registry.h"
#include "us3_turbo/client/types.h"

class cuObjClient;  // forward declaration from cuobjclient.h

namespace us3_turbo::client {

/**
 * @brief 进程唯一的 GDS 内存管理器 + RDMA token 颁发器。
 *
 * 注册表/锁/幂等注册流程继承自 BufferRegistry<size_t>(共享骨架),
 * 真正 pin 进 BAR1 的 cuObj 逻辑在 DoRegister/DoUnregister 里实现。
 * GDS 专属的 AcquireToken / Token RAII 留在本类,不进基类。
 */
class GdsMemoryManager : public BufferRegistry<std::size_t> {
 public:
  /** @brief RDMA token 的 RAII 持有者。析构调 cuMemObjPutRDMAToken 释放。 */
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
   * @brief 获取进程唯一实例。失败返回 false。
   */
  [[nodiscard]] static bool Instance(GdsMemoryManager*& out);

  /** 显式注册 device buffer（pin 入 BAR1），幂等。 */
  [[nodiscard]] bool RegisterBuffer(void* ptr, std::size_t size);

  /** 显式注销，必须在 cudaFree(ptr) 前调用，幂等。 */
  [[nodiscard]] bool UnregisterBuffer(void* ptr);

  /**
   * @brief 获取 RDMA token（RAII 析构自动释放）。
   * 未注册的 ptr 会 lazy register。
   */
  [[nodiscard]] bool AcquireToken(const void* ptr, std::size_t size,
                                       std::size_t offset, Token& out);

  GdsMemoryManager(const GdsMemoryManager&)            = delete;
  GdsMemoryManager& operator=(const GdsMemoryManager&) = delete;

 private:
  GdsMemoryManager();
  ~GdsMemoryManager() override;

  // BufferRegistry<size_t> 钩子:真正 pin 进 BAR1 / 释放。
  [[nodiscard]] bool DoRegister(void* ptr, std::size_t size,
                                std::size_t& out) override;
  void DoUnregister(void* ptr, std::size_t& handle) override;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool                  connected_{false};
};

}  // namespace us3_turbo::client
