#pragma once

#include <cstddef>

namespace us3_turbo::client {

/**
 * @brief GDS device buffer 的 RAII 包装:保证 UnregisterBuffer 先于
 *        cudaFree 执行,避免 pin 注册表残留可被地址复用命中的 stale 项。
 *        可选使用——不使用本类、继续裸 cudaMalloc/cudaFree 依然受 Step1
 *        的 buffer_id 校验兜底保护(见 review/fix_gds_pin_lifecycle.md)。
 */
class GdsDeviceBuffer {
 public:
  GdsDeviceBuffer() = default;

  /** @brief cudaMalloc(size) 并写入 out;失败返回 false,out 置空。 */
  [[nodiscard]] static bool Allocate(std::size_t size, GdsDeviceBuffer& out);

  ~GdsDeviceBuffer();

  GdsDeviceBuffer(GdsDeviceBuffer&& other) noexcept;
  GdsDeviceBuffer& operator=(GdsDeviceBuffer&& other) noexcept;
  GdsDeviceBuffer(const GdsDeviceBuffer&) = delete;
  GdsDeviceBuffer& operator=(const GdsDeviceBuffer&) = delete;

  [[nodiscard]] void* data() const noexcept { return ptr_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }

 private:
  void Reset() noexcept;  // UnregisterBuffer(先) + cudaFree(后),幂等。

  void* ptr_{nullptr};
  std::size_t size_{0};
};

}  // namespace us3_turbo::client
