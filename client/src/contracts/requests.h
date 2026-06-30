#pragma once

#include <cstdint>
#include <string>

namespace us3_turbo::client {

/**
 * @brief GdsPut / RdmaPut RPC 成功后回填的结果。
 *
 * 保留 backend 实际填写且 client 后续用到的 etag 与 crc32c；
 * bytes_transferred 由 buffer.size 推导（单对象 PUT 恒等于 buffer 大小），
 * 不随 RPC 回传。crc32c 用于 verify_crc32c 端到端校验。
 */
struct GdsPutResult {
  std::string  etag;
  std::uint32_t crc32c{0};
};

}  // namespace us3_turbo::client
