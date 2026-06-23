#include "gds/buffer_pool.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace us3_turbo::gateway::data_flow::gds {

PinnedBufferLease::PinnedBufferLease(PinnedBufferPool* pool, void* data,
                                     std::size_t capacity, rdma_buffer* mr,
                                     std::size_t size_class)
    : pool_(pool),
      data_(data),
      capacity_(capacity),
      mr_(mr),
      size_class_(size_class) {}

PinnedBufferLease::~PinnedBufferLease() { Release(); }

PinnedBufferLease::PinnedBufferLease(PinnedBufferLease&& other) noexcept
    : pool_(other.pool_),
      data_(other.data_),
      capacity_(other.capacity_),
      mr_(other.mr_),
      size_class_(other.size_class_) {
  other.pool_ = nullptr;
  other.data_ = nullptr;
  other.capacity_ = 0;
  other.mr_ = nullptr;
  other.size_class_ = 0;
}

PinnedBufferLease& PinnedBufferLease::operator=(PinnedBufferLease&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Release();
  pool_ = other.pool_;
  data_ = other.data_;
  capacity_ = other.capacity_;
  mr_ = other.mr_;
  size_class_ = other.size_class_;
  other.pool_ = nullptr;
  other.data_ = nullptr;
  other.capacity_ = 0;
  other.mr_ = nullptr;
  other.size_class_ = 0;
  return *this;
}

void PinnedBufferLease::Release() {
  if (data_ == nullptr) {
    return;
  }
  if (pool_ != nullptr) {
    pool_->Release(data_, capacity_, mr_, size_class_);
  } else {
    // 没有 pool 接收（oversize 路径 / pool 已 shutdown），直接销毁
    // 这里没法访问 server，只能 free host buffer 并漏掉 MR；调用方应已经
    // 先 deRegisterBuffer。Release 由 pool 的 Release/Dispose 主导，因此
    // 走到这条路径只有 oversize lease（pool_ == nullptr）的情况。
    std::free(data_);
  }
  pool_ = nullptr;
  data_ = nullptr;
  capacity_ = 0;
  mr_ = nullptr;
  size_class_ = 0;
}

PinnedBufferPool::PinnedBufferPool(cuObjServer& server,
                                   std::vector<std::size_t> size_classes,
                                   std::size_t max_per_class)
    : server_(server),
      size_classes_(std::move(size_classes)),
      max_per_class_(max_per_class) {
  std::sort(size_classes_.begin(), size_classes_.end());
  buckets_.reserve(size_classes_.size());
  for (std::size_t i = 0; i < size_classes_.size(); ++i) {
    buckets_.emplace_back(std::make_unique<Bucket>());
  }
}

PinnedBufferPool::~PinnedBufferPool() { Shutdown(); }

std::size_t PinnedBufferPool::PickClass(std::size_t size) const {
  for (std::size_t i = 0; i < size_classes_.size(); ++i) {
    if (size_classes_[i] >= size) {
      return i;
    }
  }
  return size_classes_.size();   // oversize
}

// 取一段 pinned + 已注册 MR 的 buffer。
// register 一次 ~500 ms（1 GiB），优先复用 free_list；超过最大 class 走 oversize。
PinnedBufferLease PinnedBufferPool::Acquire(std::size_t size) {
  if (size == 0) {
    return PinnedBufferLease{};
  }
  const auto idx = PickClass(size);
  if (idx == size_classes_.size()) {
    // Oversize：不入池，lease 用越界 size_class 作为旁路标识。
    oversize_total_ << 1;
    void* data = server_.allocHostBuffer(size);
    if (data == nullptr) {
      return PinnedBufferLease{};
    }
    auto* mr = server_.registerBuffer(data, size);
    if (mr == nullptr) {
      std::free(data);
      return PinnedBufferLease{};
    }
    return PinnedBufferLease(this, data, size, mr, size_classes_.size());
  }

  const auto capacity = size_classes_[idx];
  // 复用路径：per-class mutex 隔离，各 class 互不阻塞。
  if (!shutdown_.load(std::memory_order_acquire)) {
    auto& bucket = *buckets_[idx];
    std::scoped_lock lock(bucket.mu);
    if (!bucket.free_list.empty()) {
      CachedBuffer reuse = bucket.free_list.back();
      bucket.free_list.pop_back();
      hit_total_ << 1;
      return PinnedBufferLease(this, reuse.data, reuse.capacity, reuse.mr, idx);
    }
  }

  // 未命中：锁外执行昂贵的 alloc + register，Release 时入池复用。
  miss_total_ << 1;
  void* data = server_.allocHostBuffer(capacity);
  if (data == nullptr) {
    return PinnedBufferLease{};
  }
  auto* mr = server_.registerBuffer(data, capacity);
  if (mr == nullptr) {
    std::free(data);
    return PinnedBufferLease{};
  }
  return PinnedBufferLease(this, data, capacity, mr, idx);
}

void PinnedBufferPool::Dispose(CachedBuffer& cached) {
  if (cached.mr != nullptr) {
    server_.deRegisterBuffer(cached.mr);
    cached.mr = nullptr;
  }
  if (cached.data != nullptr) {
    std::free(cached.data);
    cached.data = nullptr;
  }
  cached.capacity = 0;
}

void PinnedBufferPool::Release(void* data, std::size_t capacity, rdma_buffer* mr,
                                std::size_t size_class) {
  if (data == nullptr) {
    return;
  }
  CachedBuffer record{data, capacity, mr};

  // oversize 或 pool 已停 → 立即销毁
  if (shutdown_.load(std::memory_order_acquire) ||
      size_class >= size_classes_.size()) {
    Dispose(record);
    return;
  }
  bool destroy_now = false;
  {
    auto& bucket = *buckets_[size_class];
    std::scoped_lock lock(bucket.mu);
    if (bucket.free_list.size() >= max_per_class_) {
      destroy_now = true;
    } else {
      bucket.free_list.push_back(record);
    }
  }
  if (destroy_now) {
    Dispose(record);
  }
}

void PinnedBufferPool::Shutdown() {
  bool expected = false;
  if (!shutdown_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
    return;
  }
  std::vector<CachedBuffer> to_destroy;
  for (auto& bucket_ptr : buckets_) {
    auto& bucket = *bucket_ptr;
    std::scoped_lock lock(bucket.mu);
    for (auto& entry : bucket.free_list) {
      to_destroy.push_back(entry);
    }
    bucket.free_list.clear();
  }
  for (auto& entry : to_destroy) {
    Dispose(entry);
  }
}

}  // namespace us3_turbo::gateway::data_flow::gds
