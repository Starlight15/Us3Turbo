#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace us3_turbo::client {

/**
 * @brief 数据流类型。保留为单值,使线协议串("gds-cuobject")成为类型常量
 *        而非裸字面量(proxy 会校验)。
 */
enum class DataFlow {
  GPUDirect,
};

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


/** @brief 返回数据流的稳定字符串标识。 */
[[nodiscard]] inline std::string_view ToString(DataFlow flow) {
  switch (flow) {
    case DataFlow::GPUDirect:
      return "gds-cuobject";
  }
  return "unknown";
}

}  // namespace us3_turbo::client
