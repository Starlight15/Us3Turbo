#pragma once

#include <cstddef>
#include <cstdint>

#include <cuobjserver.h>

namespace us3_turbo::gateway::data_flow::gds {

/**
 * @brief RAII：自动 deRegisterBuffer。
 *
 * cuObjServer::registerBuffer 把一段 host buffer pin 给 RDMA NIC，
 * 必须与 deRegisterBuffer 严格成对，否则 RDMA MR 泄漏。
 */
class RegistrationGuard {
 public:
  RegistrationGuard(cuObjServer& server, rdma_buffer* buffer)
      : server_(server), buffer_(buffer) {}
  ~RegistrationGuard() {
    if (buffer_ != nullptr) {
      server_.deRegisterBuffer(buffer_);
    }
  }
  RegistrationGuard(const RegistrationGuard&) = delete;
  RegistrationGuard& operator=(const RegistrationGuard&) = delete;

 private:
  cuObjServer& server_;
  rdma_buffer* buffer_;
};

/**
 * @brief RAII：自动 freeChannelId。
 */
class ChannelGuard {
 public:
  ChannelGuard(cuObjServer& server, std::uint16_t channel)
      : server_(server), channel_(channel) {}
  ~ChannelGuard() {
    if (channel_ != INVALID_CHANNEL_ID) {
      server_.freeChannelId(channel_);
    }
  }
  ChannelGuard(const ChannelGuard&) = delete;
  ChannelGuard& operator=(const ChannelGuard&) = delete;

 private:
  cuObjServer&  server_;
  std::uint16_t channel_;
};

/**
 * @brief RAII：cuObjServer::allocHostBuffer 拿到的 4KB 对齐 host buffer。
 *
 * cuObjServer 没有暴露对应的 freeHostBuffer API；header 注释里说
 * "user can use any host buffer and this API is optional"，
 * 实测 alloc 返回的是 posix_memalign(...) / malloc(...) 出来的指针，
 * 因此用 std::free 释放是匹配的（不能用 cudaFreeHost 之类）。
 *
 * 这里把这一约定收成 RAII 防止漏 free。
 */
class HostBuffer {
 public:
  HostBuffer(cuObjServer& server, std::size_t size) : size_(size) {
    data_ = server.allocHostBuffer(size_);
  }
  ~HostBuffer();

  HostBuffer(const HostBuffer&) = delete;
  HostBuffer& operator=(const HostBuffer&) = delete;

  [[nodiscard]] void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  void*       data_{nullptr};
  std::size_t size_{0};
};

}  // namespace us3_turbo::gateway::data_flow::gds
