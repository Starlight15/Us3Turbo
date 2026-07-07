#pragma once

// tcp_connection.h — TCP 长连接封装（阻塞 IO + 超时 + 自动重连）。
//
// 用于 proxy → backend (ufile-ac) 的自定义二进制协议通信。
// SendAll/RecvAll 保证收发完整字节；连接断开时 Close，下次 SendAll 自动重连。
//
// 注意：单个连接非线程安全——并发的 SendAll/RecvAll 会串包。BackendGateway
// 持有本类实例并用 std::mutex 序列化请求-响应对（连接池优化见 review 文档 §9.3）。

#include <cstddef>
#include <string>

namespace us3_turbo::proxy {

class TcpConnection {
 public:
  TcpConnection(const std::string& host, int port, int timeout_ms);
  ~TcpConnection();

  // 禁止拷贝
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  // 发送完整数据（阻塞直到全部发送或超时）。未连接时自动重连。
  [[nodiscard]] bool SendAll(const void* data, std::size_t size, std::string& err);

  // 接收完整数据（阻塞直到收满或超时）。必须已连接。
  [[nodiscard]] bool RecvAll(void* data, std::size_t size, std::string& err);

  // 是否已连接
  [[nodiscard]] bool IsConnected() const { return fd_ >= 0; }

 private:
  bool Connect(std::string& err);
  void Close();

  std::string host_;
  int port_;
  int timeout_ms_;
  int fd_;
};

}  // namespace us3_turbo::proxy
