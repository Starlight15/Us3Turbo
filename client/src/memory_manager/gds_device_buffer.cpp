#include "us3_turbo/client/gds_device_buffer.h"

#include "client/src/memory_manager/gds_memory_manager.h"
#include "us3_turbo/common/logger.h"

#include <cuda_runtime.h>

namespace us3_turbo::client {

bool GdsDeviceBuffer::Allocate(std::size_t size, GdsDeviceBuffer& out) {
  if (size == 0U) {
    LOG_SYS_WARN("size must be > 0");
    return false;
  }
  void* ptr = nullptr;
  const cudaError_t e = cudaMalloc(&ptr, size);
  if (e != cudaSuccess) {
    LOG_SYS_ERROR("cudaMalloc failed ({})", cudaGetErrorString(e));
    return false;
  }
  out.Reset();  // out 之前若持有其它 buffer,先按规范释放
  out.ptr_ = ptr;
  out.size_ = size;
  return true;
}

GdsDeviceBuffer::~GdsDeviceBuffer() { Reset(); }

GdsDeviceBuffer::GdsDeviceBuffer(GdsDeviceBuffer&& other) noexcept
    : ptr_(other.ptr_), size_(other.size_) {
  other.ptr_ = nullptr;
  other.size_ = 0;
}

GdsDeviceBuffer& GdsDeviceBuffer::operator=(GdsDeviceBuffer&& other) noexcept {
  if (this != &other) {
    Reset();
    ptr_ = other.ptr_;
    size_ = other.size_;
    other.ptr_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void GdsDeviceBuffer::Reset() noexcept {
  if (ptr_ == nullptr) return;
  GdsMemoryManager* mgr = nullptr;
  if (GdsMemoryManager::Instance(mgr) && mgr != nullptr) {
    // 幂等:ptr_ 未曾 AcquireToken 过也直接返回 true。
    if (!mgr->UnregisterBuffer(ptr_)) {
      LOG_SYS_ERROR(
          "UnregisterBuffer failed (ptr={}), stale entry may remain until "
          "process exit. Relying on buffer_id check for safety.",
          ptr_);
    }
  } else {
    // manager 不可用(例如进程退出路径):无法主动 unregister,
    // 退化依赖 Step1 的 buffer_id 校验在下次复用时兜底识别。
    LOG_SYS_DEBUG("GdsMemoryManager unavailable (ptr={}), skip unregister", ptr_);
  }
  const cudaError_t e = cudaFree(ptr_);
  if (e != cudaSuccess) {
    LOG_SYS_ERROR("cudaFree failed (ptr={} err={})", ptr_, cudaGetErrorString(e));
  }
  ptr_ = nullptr;
  size_ = 0;
}

}  // namespace us3_turbo::client
