#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace us3_turbo::client {

/**
 * @brief 附着到每次 RPC 的 header / 鉴权 / 超时上下文。
 *
 * client-new 自有定义,不复用旧 client 的 RpcCallMetadata。字段与
 * ClientOptions 中对应的鉴权 / 头 / 超时一一对应。
 */
struct RpcCallMetadata {
  std::string client_id;
  std::string bearer_token;
  std::unordered_map<std::string, std::string> default_headers;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
};

}  // namespace us3_turbo::client
