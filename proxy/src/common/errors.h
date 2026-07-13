#pragma once

#include <string>

namespace us3_turbo::proxy {

/* proxy 控制面统一错误码
 * - 10xxx：参数错误
 * - 12xxx：backend 错误
 * - 13xxx：路由 / data_flow 错误
 */
constexpr int PROXY_ERR_INVALID_PARAM        = 10001;
constexpr int PROXY_ERR_INVALID_PART_SIZE    = 10002;  // part 大小不符合 16MB 约束
constexpr int PROXY_ERR_INVALID_PART         = 10003;  // part 无效（valid=false）
constexpr int PROXY_ERR_INDEX_FAILED         = 10004;  // 索引写入失败
constexpr int PROXY_ERR_INTERNAL             = 10099;  // 内部错误（索引操作失败等）
constexpr int PROXY_ERR_BACKEND_UNAVAILABLE  = 12001;
constexpr int PROXY_ERR_BACKEND_RPC          = 12002;
constexpr int PROXY_ERR_BACKEND_IO           = 12003;  // backend TCP IO failure
constexpr int PROXY_ERR_BACKEND_PROTOCOL     = 12004;  // backend protocol decode error
constexpr int PROXY_ERR_BACKEND_FAILED       = 12005;
constexpr int PROXY_ERR_PATH_NOT_SUPPORTED   = 13002;  // path 与 RPC 不匹配 / kAll/kNone
constexpr int PROXY_ERR_MISSING_SOURCE       = 13003;  // path 指定但对应 source 缺失

/* 错误码通用描述 */
inline const char* ProxyErrorMessage(int code) {
  switch (code) {
    case PROXY_ERR_INVALID_PARAM:       return "invalid parameter";
    case PROXY_ERR_INVALID_PART_SIZE:   return "invalid part size";
    case PROXY_ERR_INVALID_PART:        return "invalid part";
    case PROXY_ERR_INDEX_FAILED:        return "index write failed";
    case PROXY_ERR_INTERNAL:            return "internal error";
    case PROXY_ERR_BACKEND_UNAVAILABLE: return "backend unavailable";
    case PROXY_ERR_BACKEND_RPC:         return "backend RPC failed";
    case PROXY_ERR_BACKEND_IO:          return "backend IO failed";
    case PROXY_ERR_BACKEND_PROTOCOL:    return "backend protocol error";
    case PROXY_ERR_BACKEND_FAILED:      return "backend returned error";
    case PROXY_ERR_PATH_NOT_SUPPORTED:  return "path not supported";
    case PROXY_ERR_MISSING_SOURCE:      return "missing source field";
    default:                            return "unknown error";
  }
}

}  // namespace us3_turbo::proxy
