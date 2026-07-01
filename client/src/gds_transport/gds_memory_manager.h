#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "us3_turbo/client/types.h"

class cuObjClient;  // forward declaration from cuobjclient.h

namespace us3_turbo::client {

/**
 * @brief 进程唯一的 GDS 内存管理器 + RDMA token 颁发器。
 */
class GdsMemoryManager {
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
  ~GdsMemoryManager();

  [[nodiscard]] bool RegisterBufferUnderLock(void* ptr, std::size_t size);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool                  connected_{false};

  std::mutex                             registration_mu_;
  std::unordered_map<void*, std::size_t> registered_;
};

}  // namespace us3_turbo::client
