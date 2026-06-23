#pragma once

#include <chrono>

namespace us3_turbo::client {

/**
 * @brief 附着到每次 RPC 的超时上下文。
 *
 * client-new 自有定义。鉴权 / 自定义 header 在 GDS-only 链路无服务端消费
 * (proxy / backend 不读 Authorization / x-fa-client-id / default_headers),
 * 已移除;只保留 RPC 超时。
 */
struct RpcCallMetadata {
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
};

}  // namespace us3_turbo::client
