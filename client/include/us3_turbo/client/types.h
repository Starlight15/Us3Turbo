#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace us3_turbo::client {

/**
 * @brief Data flow type. The GDS-only client always uses GPUDirect; the enum
 *        is kept as a single value so the wire string ("gds-cuobject") stays
 *        a typed constant rather than a bare literal (proxy validates it).
 */
enum class DataFlow {
  GPUDirect,
};

/**
 * @brief Read-only data buffer supplied to upload operations.
 *
 * GDS PUT 链路要求 device buffer;data 指向 cudaMalloc 的显存,调用返回前
 * 必须保持有效。UCX PUT 链路则要求 host 内存。
 */
struct ConstBufferView {
  const void* data{nullptr};
  std::size_t size{0};
};


/** @brief Returns a stable string identifier for the data flow. */
[[nodiscard]] inline std::string_view ToString(DataFlow flow) {
  switch (flow) {
    case DataFlow::GPUDirect:
      return "gds-cuobject";
  }
  return "unknown";
}

}  // namespace us3_turbo::client
