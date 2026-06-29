#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace us3_turbo::client {

/**
 * @brief Client instance configuration.
 *
 * Mode B：client 只与 proxy 交互（GdsPut / RdmaPut 都走 options.endpoint），
 * backend 对 client 透明。client_id / bearer_token / default_headers /
 * gds_data_endpoint 在 GDS-only 链路无服务端消费，已移除。只保留 PUT
 * 必需的 endpoint / 超时 / 大小上限配置。
 */
struct ClientOptions {
  /** proxy 控制面 endpoint "host:port"（GdsPut / RdmaPut 都走这里）。 */
  std::string endpoint;
  /** Default per-channel timeout（用于 MetaRpc / ChunkRpc channel Init）。 */
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};
  /** 端到端单笔 PUT 超时。 */
  std::chrono::milliseconds request_timeout{std::chrono::minutes(5)};

  /**
   * 单次 PUT 的上限（client 入口拒）。与 gateway 端 cuObjServer 的
   * 1 GiB chunk 上限对齐，避免发到 server 才被拒。0 表示不限。
   */
  std::size_t put_single_max_bytes{1ULL * 1024 * 1024 * 1024};

  /**
   * 端到端 CRC32C 一致性校验开关。开启后 PutObject 会在 GdsPut 成功后,
   * 把 device buffer 拷回 host 计算 CRC32C,并与 backend 在
   * GdsChunkResponse.crc32c 回传的值比对:
   *   - 一致: spdlog::info 记录 MATCH;
   *   - 不一致: spdlog::error 记录 MISMATCH 并视为失败(返回 false,触发重试)。
   * 默认关闭。开启会引入一次 D2H 拷贝 + 软件计算开销,仅用于一致性验证。
   */
  bool verify_crc32c{false};

  /**
   * 单次 PUT 阶段耗时埋点开关。开启后 PutOnce 按 AcquireToken /
   * GdsPut 分段计时并 spdlog::info 输出,用于定位性能瓶颈。默认关闭,关闭时
   * 不产生任何计时/日志开销。
   */
  bool latency_trace{false};
};

}  // namespace us3_turbo::client
