#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include <bvar/bvar.h>
#include <cuobjserver.h>

namespace us3_turbo::gateway::data_flow::gds {

/**
 * @brief 一段已经 pin 给 cuObjServer 并注册过 RDMA MR 的 host buffer。
 *
 * Lease 析构会把 buffer 还给 pool；如果 pool 已不接收（满或已 Shutdown），
 * lease 内部直接 dispose（deRegisterBuffer + free）。
 */
class PinnedBufferPool;

class PinnedBufferLease {
 public:
  PinnedBufferLease() = default;
  PinnedBufferLease(PinnedBufferPool* pool, void* data, std::size_t capacity,
                    rdma_buffer* mr, std::size_t size_class);
  ~PinnedBufferLease();

  PinnedBufferLease(const PinnedBufferLease&) = delete;
  PinnedBufferLease& operator=(const PinnedBufferLease&) = delete;
  PinnedBufferLease(PinnedBufferLease&& other) noexcept;
  PinnedBufferLease& operator=(PinnedBufferLease&& other) noexcept;

  [[nodiscard]] void*       data()       noexcept { return data_; }
  [[nodiscard]] const void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] rdma_buffer* mr()  const noexcept { return mr_; }
  [[nodiscard]] bool ok()         const noexcept { return data_ != nullptr && mr_ != nullptr; }

 private:
  void Release();

  PinnedBufferPool* pool_{nullptr};
  void*             data_{nullptr};
  std::size_t       capacity_{0};
  rdma_buffer*      mr_{nullptr};
  std::size_t       size_class_{0};
};

/**
 * @brief 按 size class 缓存 pinned + RDMA-registered host buffer。
 *
 * size_classes 在构造时给定，比如 {1MiB, 16MiB, 256MiB, 1GiB}；Acquire(n) 会
 * 选择第一个 ≥ n 的 class；超过最大 class 的请求直接 alloc 一个临时的
 * （不入池，归还时直接 free）。
 *
 * 每 class 最多缓存 max_per_class 个空闲 lease，超过的 lease 归还时被销毁。
 *
 * pool 用一把 std::mutex 保护各 class 的空闲列表；Acquire/Release 是 O(1)。
 */
class PinnedBufferPool {
 public:
  PinnedBufferPool(cuObjServer& server, std::vector<std::size_t> size_classes,
                   std::size_t max_per_class);
  ~PinnedBufferPool();

  PinnedBufferPool(const PinnedBufferPool&) = delete;
  PinnedBufferPool& operator=(const PinnedBufferPool&) = delete;

  [[nodiscard]] PinnedBufferLease Acquire(std::size_t size);

  /**
   * @brief 释放所有缓存的 buffer；之后 Acquire 会重新分配。
   *        Shutdown 后归还的 lease 会被立即销毁，不再进池。
   */
  void Shutdown();

 private:
  friend class PinnedBufferLease;

  struct CachedBuffer {
    void*        data{nullptr};
    std::size_t  capacity{0};
    rdma_buffer* mr{nullptr};
  };

  [[nodiscard]] std::size_t PickClass(std::size_t size) const;
  void Release(void* data, std::size_t capacity, rdma_buffer* mr,
                std::size_t size_class);
  void Dispose(CachedBuffer& cached);

  struct Bucket {
    mutable std::mutex            mu;
    std::vector<CachedBuffer>     free_list;
  };

  cuObjServer&                        server_;
  std::vector<std::size_t>            size_classes_;   // sorted ascending
  std::size_t                         max_per_class_;
  std::vector<std::unique_ptr<Bucket>> buckets_;        // 每 size class 一个 bucket，独立 mutex
  std::atomic<bool>                   shutdown_{false};

  bvar::Adder<std::int64_t>           hit_total_{"gateway_pinned_buffer_pool_hit_total"};
  bvar::Adder<std::int64_t>           miss_total_{"gateway_pinned_buffer_pool_miss_total"};
  bvar::Adder<std::int64_t>           oversize_total_{"gateway_pinned_buffer_pool_oversize_total"};
};

}  // namespace us3_turbo::gateway::data_flow::gds
