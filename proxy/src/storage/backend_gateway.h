#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "proxy/src/common/errors.h"
#include "proxy/src/service/single_put.h"  // PutOutput
#include "proxy/src/storage/tcp_connection.h"

namespace us3_turbo::proxy {

// 单步转发存储层：自持到 backend (ufile-ac) 的 TCP 长连接，走自定义二进制协议
//（见 backend_protocol.h）。GDS/UCX 各独立方法；参数校验留在 SinglePut，本类只管转发。
//
// 并发：单个 TCP 连接非线程安全，brpc 多 worker 并发调用时用 conn_mu_ 序列化
// 请求-响应对（保证正确性；连接池优化见 review/proxy_backend_protocol_migration.md §9.3）。
//
// 成功返回 0，失败先 LOG_* 再 return 错误码。rid 从 request.request_id() 取。
class BackendGateway {
 public:
  BackendGateway(const std::string& backend_endpoint, int timeout_ms,
                 std::uint32_t setid);

  [[nodiscard]] int ForwardGdsPut(
      const ::us3_turbo::proxy::ClientProxyPutRequest& request,
      PutOutput& out);
  [[nodiscard]] int ForwardUcxPut(
      const ::us3_turbo::proxy::ClientProxyPutRequest& request,
      PutOutput& out);

 private:
  // 拆分 "host:port" → host + port；失败返回 false。
  static bool ParseEndpoint(const std::string& endpoint,
                            std::string& host, int& port);

  int                timeout_ms_;
  std::uint32_t      setid_;
  std::unique_ptr<TcpConnection> conn_;
  std::mutex         conn_mu_;   // 序列化并发请求-响应对
};

}  // namespace us3_turbo::proxy
