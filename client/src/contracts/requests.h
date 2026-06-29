#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace us3_turbo::client {

/**
 * @brief 一次 PUT 尝试的唯一上下文。
 *
 * bucket / key / timeout / request_id 在整条链路只存这一份，GdsPut / RdmaPut
 * 直接从它读取，不再逐层复制。length 仍来自 PutObjectRequest.expected_size，
 * 用于 proxy 的 chunk_size 校验。
 *
 * timeout 用 request.timeout，非正数时回退到 options.request_timeout；
 * request_id 由工厂每次生成，故每次重试都会拿到新的 id。
 */
struct PutAttempt {
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::string bucket;
  std::string key;
  std::optional<std::uint64_t> length;
  std::string request_id;
};

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
