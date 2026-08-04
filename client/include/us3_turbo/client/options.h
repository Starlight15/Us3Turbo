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

  // RPC 超时
  std::chrono::milliseconds rpc_timeout{std::chrono::milliseconds(30000)};

  // 单步 PUT 对象上限，默认 4MiB。0 表示不限制。
  std::size_t put_single_max_bytes{4ULL * 1024 * 1024};

  // 分段上传 part 大小，默认 4MiB，须与 proxy FLAGS_multipart_part_size 一致。
  // 非 last part 必须恰好等于此值；仅 last part 可小于此值。
  // 违反此规则将在 CompleteMultipartUpload 时被 proxy 拒绝。
  std::size_t multipart_part_size{4ULL * 1024 * 1024};

  // 端到端 CRC32C 校验开关
  bool verify_crc32c{false};

  // 单次 PUT 阶段耗时埋点开关，默认关闭
  bool latency_trace{false};

  // 单步 PUT 失败后的重试退避（retry-once），默认 100ms
  std::chrono::milliseconds retry_backoff{std::chrono::milliseconds(100)};

  // 日志级别: "debug"/"info"/"warn"/"error"，默认 info
  std::string log_level{"info"};

  // RDMA CM listener 绑定 IP。空串表示从环境变量 US3_TURBO_RDMA_BIND_IP 读取；
  // 仍为空则用内置 fallback（仅适配本测试环境）。须匹配本地 RDMA 数据网卡 IP，
  // 否则 device() 返回 null。多 NIC 机器请通过环境变量或显式设值指定数据网卡。
  std::string rdma_bind_ip;
};

}  // namespace us3_turbo::client
