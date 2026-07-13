#pragma once

#include <cstddef>

namespace us3_turbo::client {

/** @brief 上传用的只读数据缓冲区(GDS=device 显存,UCX=host 内存)。 */
struct ConstBufferView {
  const void* data{nullptr};
  std::size_t size{0};
};

/** @brief GET 用的可写数据缓冲区(GDS=device 显存)。 */
struct MutableBufferView {
  void* data{nullptr};
  std::size_t size{0};
};

}  // namespace us3_turbo::client
