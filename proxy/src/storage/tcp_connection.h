#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>  // std::move

namespace us3_turbo::proxy {

// TCP 长连接封装（阻塞 IO + 超时 + 原子存活标记）。用于 proxy → backend
// (ufile-ac) 自定义二进制协议通信。单连接非线程安全 —— UfileAcClient 用每连接
// 独立 mutex 序列化请求-响应对；断开置 dead，由 AcquireConn 惰性重连（方案 A）。
class TcpConnection {
 public:
  TcpConnection(std::string host, int port, int timeout_ms)
      : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), fd_(-1) {}
  ~TcpConnection() { Close(); }
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  // 建 socket + connect + 设 SO_RCVTIMEO/SO_SNDTIMEO。已连接返回 true（幂等）；
  // 失败置 dead 并返回 false。
  bool Connect();
  void Close();

  // 0=完整收发成功，-1=失败（已 set_dead + Close）。短读/短写内部补齐。
  int SendAll(const void* buf, std::size_t len);
  int RecvAll(void* buf, std::size_t len);

  bool alive() const { return alive_.load(std::memory_order_acquire); }
  void set_dead() { alive_.store(false, std::memory_order_release); }

 private:
  std::string       host_;
  int               port_;
  int               timeout_ms_;
  int               fd_{-1};
  std::atomic<bool> alive_{false};
};

}  // namespace us3_turbo::proxy
