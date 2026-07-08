#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>  // std::pair
#include <vector>

#include "control_plane.pb.h"          // ClientProxyPutRequest
#include "proxy/src/common/errors.h"
#include "proxy/src/service/single_put.h"  // PutOutput

namespace us3_turbo::proxy {

// TCP 长连接封装（阻塞 IO + 超时 + 原子存活标记）。用于 proxy → backend
// (ufile-ac) 自定义二进制协议通信。单连接非线程安全 —— BackendGateway 用每连接
// 独立 mutex 序列化请求-响应对；断开置 dead，由 AcquireConn 惰性重连（方案 A）。
class TcpConnection {
 public:
  TcpConnection(std::string host, int port, int timeout_ms);
  ~TcpConnection();
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

// 单步转发存储层：自持到 backend (ufile-ac) 的 TCP 连接池（轮询 + 惰性重连，
// 方案 A），走自定义二进制协议（见 ufile_ac_protocol.h）。GDS/UCX 各独立方法；
// 参数校验留在 SinglePut，本类只管序列化 + 转发（doc §限制2）。
//
// 并发：每连接独立 mutex（vector<unique_ptr<mutex>>，mutex 不可移动故用 unique_ptr）
// 序列化请求-响应对，保证不串包；next_idx_ atomic 轮询分配。连接断开 set_dead，
// AcquireConn 跳过并当场重连一次，全坏返回 nullptr → PROXY_ERR_BACKEND_UNAVAILABLE。
//
// 成功返回 0，失败先 LOG_* 再 return 错误码。rid 从 request.request_id() 取。
class BackendGateway {
 public:
  BackendGateway(const std::string& backend_endpoint, int timeout_ms);

  [[nodiscard]] int ForwardGdsPut(
      const ClientProxyPutRequest& request,
      PutOutput& out);
  [[nodiscard]] int ForwardUcxPut(
      const ClientProxyPutRequest& request,
      PutOutput& out);

 private:
  // 拆分 "host:port" → host + port；失败返回 false。
  static bool ParseEndpoint(const std::string& endpoint,
                            std::string& host, int& port);
  // 方案 A 惰性取连接：轮询最多 pool_size 次，跳过/重连坏连接。
  // 返回 {idx, conn*}；全坏返回 {npos, nullptr}。
  std::pair<std::size_t, TcpConnection*> AcquireConn();

  int                                        timeout_ms_;
  std::uint32_t                              setid_;
  std::string                                host_;
  int                                        port_{0};
  std::vector<std::unique_ptr<TcpConnection>> conns_;
  std::vector<std::unique_ptr<std::mutex>>    conn_mutexes_;
  std::atomic<std::uint64_t>                 next_idx_{0};      // 轮询计数器
  std::atomic<std::uint64_t>                 session_seq_{0};   // sessionIdLow_ 自增（统计用）
};

}  // namespace us3_turbo::proxy
