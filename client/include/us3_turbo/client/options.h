#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace us3_turbo::client {

/**
 * @brief Client 实例配置。
 *
 * Mode B:client 只与 proxy 交互(GdsPut / UcxPut 都走 endpoint),backend 对
 * client 透明。只保留 PUT 必需的 endpoint / 超时 / 大小上限配置。
 */
struct ClientOptions {
  /** proxy 控制面 endpoint "host:port"。 */
  std::string endpoint;
  /** 单 channel 默认超时(RpcBase Init + ApplyTimeout)。 */
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};

  /** 单次 PUT 上限(client 入口拒),与 backend 1 GiB chunk 对齐。0 表示不限。 */
  std::size_t put_single_max_bytes{1ULL * 1024 * 1024 * 1024};

  /**
   * @brief 端到端 CRC32C 校验开关。开启后比对本地 CRC32C 与 backend 回传值,
   *        不一致则记 error 并判失败。默认关闭(GDS 通路额外一次 D2H 拷贝)。
   */
  bool verify_crc32c{false};

  /** 单次 PUT 阶段耗时埋点开关(按 Acquire/Put 分段计时),默认关闭。 */
  bool latency_trace{false};
};

}  // namespace us3_turbo::client
