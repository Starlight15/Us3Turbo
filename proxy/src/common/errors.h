#pragma once

namespace us3_turbo::proxy {

/**
 * @brief proxy 控制面统一错误码（供 cntl->SetFailed() 使用）。
 *
 * 不使用字符串错误码（brpc 支持 int）。错误码分段：
 * - 10xxx：参数错误
 * - 12xxx：backend 错误
 * - 13xxx：路由 / data_flow 错误
 */
constexpr int PROXY_ERR_INVALID_PARAM        = 10001;
constexpr int PROXY_ERR_BACKEND_UNAVAILABLE  = 12001;
constexpr int PROXY_ERR_BACKEND_RPC          = 12002;
constexpr int PROXY_ERR_PATH_NOT_SUPPORTED   = 13002;  // path 与 RPC 不匹配 / kAll/kNone
constexpr int PROXY_ERR_MISSING_SOURCE       = 13003;  // path 指定但对应 source 缺失

}  // namespace us3_turbo::proxy
