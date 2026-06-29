#pragma once

#include <cstddef>

#include <ucp/api/ucp.h>

namespace us3_turbo::backend::rdma {

/**
 * @brief UCX ep 的 RAII 守卫：析构时 ucp_ep_close_nbx + progress 到完成。
 *
 * 与 gds/cuobj_resources.h 的 ChannelGuard 平行、独立，不复用。
 * ep_close_nbx 可能返回请求句柄（非内联完成），需在所属 worker 上
 * progress 直到完成再 ucp_request_free。
 */
class EpGuard {
 public:
  EpGuard(ucp_worker_h worker, ucp_ep_h ep) noexcept
      : worker_(worker), ep_(ep) {}
  ~EpGuard();
  EpGuard(const EpGuard&) = delete;
  EpGuard& operator=(const EpGuard&) = delete;
  EpGuard(EpGuard&& o) noexcept : worker_(o.worker_), ep_(o.ep_) {
    o.ep_ = nullptr;
  }
  EpGuard& operator=(EpGuard&&) = delete;

  [[nodiscard]] ucp_ep_h ep() const noexcept { return ep_; }
  [[nodiscard]] bool valid() const noexcept { return ep_ != nullptr; }

 private:
  ucp_worker_h worker_{nullptr};
  ucp_ep_h     ep_{nullptr};
};

/**
 * @brief UCX rkey 的 RAII 守卫：析构时 ucp_rkey_destroy。
 *
 * 与 cuObjServer 的 token 不同：UCX 的 rkey 是在 puller 侧
 * ucp_ep_rkey_unpack 得到的本地句柄，用完必须 destroy。
 */
class RkeyGuard {
 public:
  explicit RkeyGuard(ucp_rkey_h rkey) noexcept : rkey_(rkey) {}
  ~RkeyGuard();
  RkeyGuard(const RkeyGuard&) = delete;
  RkeyGuard& operator=(const RkeyGuard&) = delete;
  RkeyGuard(RkeyGuard&& o) noexcept : rkey_(o.rkey_) { o.rkey_ = nullptr; }
  RkeyGuard& operator=(RkeyGuard&&) = delete;

  [[nodiscard]] ucp_rkey_h rkey() const noexcept { return rkey_; }
  [[nodiscard]] bool valid() const noexcept { return rkey_ != nullptr; }

 private:
  ucp_rkey_h rkey_{nullptr};
};

/**
 * @brief malloc 出来的 host buffer RAII（析构 std::free）。
 *
 * UCX rdma 链路 sink 把拉到的字节暂存进本地 malloc buffer，丢弃前算 CRC。
 * 用 malloc/free（非 cuObjServer pinned pool），因为 UCX 的 ucp_get_nbx
 * 目标 buffer 不需要 GPU-pinned，普通 host 内存即可。
 */
class HostBuffer {
 public:
  explicit HostBuffer(std::size_t size);
  ~HostBuffer();
  HostBuffer(const HostBuffer&) = delete;
  HostBuffer& operator=(const HostBuffer&) = delete;
  HostBuffer(HostBuffer&&) = delete;
  HostBuffer& operator=(HostBuffer&&) = delete;

  [[nodiscard]] void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool ok() const noexcept { return data_ != nullptr; }

 private:
  void*       data_{nullptr};
  std::size_t size_{0};
};

}  // namespace us3_turbo::backend::rdma
