#pragma once

#include <cstddef>

namespace us3_turbo::client {

/**
 * @brief 上传用的只读数据缓冲区。
 *
 * GDS 链路要求 device buffer(data 指向 cudaMalloc 显存,返回前须有效);
 * UCX 链路要求 host 内存。
 */
struct ConstBufferView {
  const void* data{nullptr};
  std::size_t size{0};
};

}  // namespace us3_turbo::client
