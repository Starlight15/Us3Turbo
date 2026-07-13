#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "client/src/memory_manager/buffer_registry.h"
#include "us3_turbo/client/types.h"

#include <cuobjclient.h>  // needed for cuObjOpType_t in AcquireToken signature

namespace us3_turbo::client {

/**
 * @brief 进程唯一的 GDS 内存管理器:注册 device buffer 并颁发 RDMA token。
 */
class GdsMemoryManager : public BufferRegistry<std::size_t> {
 public:
  /** @brief RDMA token 的 RAII 持有者,析构调 cuMemObjPutRDMAToken 释放。 */
  class Token {
   public:
    Token() = default;
    Token(Token&& other) noexcept;
    Token& operator=(Token&& other) noexcept;
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    ~Token();

    [[nodiscard]] std::string_view str() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return tok_ != nullptr; }

   private:
    friend class GdsMemoryManager;
    Token(cuObjClient* client, char* tok) noexcept
        : client_(client), tok_(tok) {}
    void Reset() noexcept;

    cuObjClient* client_{nullptr};
    char* tok_{nullptr};
  };

  /** @brief 获取进程唯一实例,失败返回 false。 */
  [[nodiscard]] static bool Instance(GdsMemoryManager*& out);

  /** @brief 显式注册 device buffer(pin 入 BAR1),幂等。 */
  [[nodiscard]] bool RegisterBuffer(void* ptr, std::size_t size);

  /** @brief 显式注销,须在 cudaFree(ptr) 前调用,幂等。 */
  [[nodiscard]] bool UnregisterBuffer(void* ptr);

  /** @brief 获取 RDMA token(RAII 析构自动释放)。未注册的 ptr 会 lazy register。
   *  operation: CUOBJ_PUT(默认，写场景 backend RDMA_READ) 或
   *  CUOBJ_GET(读场景 backend RDMA_WRITE)。 */
  [[nodiscard]] bool AcquireToken(
      const void* ptr, std::size_t size, std::size_t offset, Token& out,
      cuObjOpType_t operation = static_cast<cuObjOpType_t>(0));

  GdsMemoryManager(const GdsMemoryManager&) = delete;
  GdsMemoryManager& operator=(const GdsMemoryManager&) = delete;

 private:
  GdsMemoryManager();
  ~GdsMemoryManager() override;

  /** @brief BufferRegistry<size_t> 钩子:真正 pin 进 BAR1。 */
  [[nodiscard]] bool DoRegister(void* ptr, std::size_t size,
                                std::size_t& out) override;
  /** @brief BufferRegistry<size_t> 钩子:释放 pin(调 cuMemObjPutDescriptor)。 */
  void DoUnregister(void* ptr, std::size_t& handle) override;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool connected_{false};
};

}  // namespace us3_turbo::client
