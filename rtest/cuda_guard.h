// rtest/cuda_guard.h — GDS 回归测试/示例共用的 CUDA device buffer RAII guard。
//
// 构造即 cudaMalloc，析构即 cudaFree；移动语义支持转移所有权。替代回归测试中
// goto cleanup + 手动 cudaFree 的 C 风格，统一为 RAII + early-return。
#pragma once

#include <cstddef>

#include <cuda_runtime.h>

namespace rtest {

// RAII 守卫：持有一块 device 显存。cudaMalloc 失败时 valid()==false。
class DevMem {
 public:
  DevMem() = default;
  explicit DevMem(std::size_t size) { alloc(size); }

  DevMem(const DevMem&) = delete;
  DevMem& operator=(const DevMem&) = delete;
  DevMem(DevMem&& o) noexcept : ptr_(o.ptr_), size_(o.size_) {
    o.ptr_ = nullptr;
    o.size_ = 0;
  }
  DevMem& operator=(DevMem&& o) noexcept {
    if (this != &o) {
      free();
      ptr_ = o.ptr_;
      size_ = o.size_;
      o.ptr_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }
  ~DevMem() { free(); }

  // 分配；失败返回 false（ptr 保持 nullptr）。可重复调用以重新分配。
  bool alloc(std::size_t size) {
    free();
    if (cudaMalloc(&ptr_, size) != cudaSuccess) {
      ptr_ = nullptr;
      size_ = 0;
      return false;
    }
    size_ = size;
    return true;
  }

  void free() noexcept {
    if (ptr_) {
      cudaFree(ptr_);
      ptr_ = nullptr;
      size_ = 0;
    }
  }

  void* get() const noexcept { return ptr_; }
  std::size_t size() const noexcept { return size_; }
  bool valid() const noexcept { return ptr_ != nullptr; }

 private:
  void* ptr_{nullptr};
  std::size_t size_{0};
};

}  // namespace rtest
