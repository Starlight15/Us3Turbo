#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace us3_turbo::client {

/**
 * @brief Client 实例配置。
 */
struct ClientOptions {
  // proxy 控制面 endpoint
  std::string endpoint;

  // 默认超时
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};

  // 单步 PUT 对象上限，默认 16MiB；超出走分段上传。0 表示不限制。
  std::size_t put_single_max_bytes{16ULL * 1024 * 1024};

  // 分段上传 part 大小，默认 16MiB，须与 proxy FLAGS_multipart_part_size 一致。
  // 非 last part 必须恰好等于此值；仅 last part 可小于此值。
  // 违反此规则将在 CompleteMultipartUpload 时被 proxy 拒绝。
  std::size_t multipart_part_size{16ULL * 1024 * 1024};

  // 端到端 CRC32C 校验开关
  bool verify_crc32c{false};

  // 单次 PUT 阶段耗时埋点开关，默认关闭
  bool latency_trace{false};
};

}  // namespace us3_turbo::client
