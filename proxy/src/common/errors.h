#pragma once

#include <string_view>

namespace us3_turbo::proxy {

/**
 * @brief proxy 控制面统一错误码（供 cntl->SetFailed() 使用）。
 *
 * 不使用字符串错误码（brpc 支持 int）。错误码分段：
 * - 10xxx：参数错误
 * - 11xxx：session 错误
 * - 12xxx：backend 错误
 * - 13xxx：路由 / data_flow 错误
 * - 90xxx：占位 / 未实现
 */
constexpr int PROXY_ERR_INVALID_PARAM        = 10001;
constexpr int PROXY_ERR_SESSION_EXPIRED      = 11001;
constexpr int PROXY_ERR_SESSION_NOT_FOUND    = 11002;
constexpr int PROXY_ERR_BACKEND_UNAVAILABLE  = 12001;
constexpr int PROXY_ERR_BACKEND_RPC          = 12002;
constexpr int PROXY_ERR_UNSUPPORTED_PATH     = 13001;
constexpr int PROXY_ERR_NOT_IMPLEMENTED      = 90001;

// 错误消息（可选，用于日志）。
constexpr std::string_view ErrorMessage(int code) {
  switch (code) {
    case PROXY_ERR_INVALID_PARAM:       return "invalid parameter";
    case PROXY_ERR_SESSION_EXPIRED:     return "session expired";
    case PROXY_ERR_SESSION_NOT_FOUND:   return "session not found";
    case PROXY_ERR_BACKEND_UNAVAILABLE: return "no backend available";
    case PROXY_ERR_BACKEND_RPC:         return "backend rpc failed";
    case PROXY_ERR_UNSUPPORTED_PATH:    return "unsupported data_flow";
    case PROXY_ERR_NOT_IMPLEMENTED:     return "not implemented in proxy v1";
    default:                            return "unknown error";
  }
}

}  // namespace us3_turbo::proxy
