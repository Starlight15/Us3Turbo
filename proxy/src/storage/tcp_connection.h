#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>  // std::move

namespace us3_turbo::proxy {

/* TCP 长连接封装：阻塞 IO + 超时 + 原子存活标记。
   用于 proxy → backend (ufile-ac) 自定义二进制协议通信。
   非线程安全；UfileAcClient 每连接 mutex 序列化，断开置 dead 由 AcquireConn 惰性重连。 */
class TcpConnection {
 public:
  TcpConnection(std::string host, int port, int timeout_ms)
      : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), fd_(-1) {}
  ~TcpConnection() { Close(); }
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  /* 创建 socket 并连接，设置收发超时；已连接则幂等返回 true，失败置 dead 返回 false。 */
  bool Connect();
  /* 关闭连接并释放 fd。 */
  void Close();

  /* 完整收发指定长度；短读/短写内部补齐。成功返回 0，失败置 dead 并关闭后返回 -1。 */
  int SendAll(const void* buf, std::size_t len);
  int RecvAll(void* buf, std::size_t len);

  /* 查询连接存活状态。 */
  bool alive() const { return alive_.load(std::memory_order_acquire); }
  /* 标记连接已断开。 */
  void set_dead() { alive_.store(false, std::memory_order_release); }

 private:
  std::string       host_;
  int               port_;
  int               timeout_ms_;
  int               fd_{-1};
  std::atomic<bool> alive_{false};
};

}  // namespace us3_turbo::proxy
