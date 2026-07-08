#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>  // std::pair
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/storage/tcp_connection.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

// block 级写入结果。ret_code 为 PROXY_ERR_* 错误码（0=成功），调用方据此
// 上报；error 为失败上下文（成功时为空）。crc32c/bytes_written 仅成功时有意义。
struct BlockResult {
  int           ret_code{0};
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
  std::string   error;
};

// block 级存储客户端：自持到 backend (ufile-ac) 的 TCP 连接池（轮询 + 惰性重连，
// 方案 A），走自定义二进制协议（编解码见 protocol_codec.h）。接口收原始参数
// （key/rdma_token/gpu_offset/data_len 等），不依赖 protobuf，可被单步上传与
// 分段上传的 block 拆分复用；key 由 proxy 生成（结构化 block 标识）。
//
// 并发：每连接独立 mutex（vector<unique_ptr<mutex>>，mutex 不可移动故用 unique_ptr）
// 序列化请求-响应对，保证不串包；next_idx_ atomic 轮询分配。连接断开 set_dead，
// AcquireConn 跳过并当场重连一次，全坏返回 nullptr → ret_code=PROXY_ERR_BACKEND_UNAVAILABLE。
//
// 本层为存储边界，做 key 长度 / data_len 范围等存储入参守卫（非请求级校验，
// 请求级校验仍在 SinglePut）。日志走 LOG_SYS_*（无 rid，rid 在服务层）。
class UfileAcClient {
 public:
  UfileAcClient(const std::string& backend_endpoint, int timeout_ms);

  // GDS：写一个 block。key=block 标识（proxy 生成），gpu_offset=GPU buffer 偏移。
  [[nodiscard]] BlockResult PutBlockGds(
      const std::string& key,
      const std::string& rdma_token,
      std::uint64_t gpu_offset,
      std::uint64_t data_len);

  // UCX：写一个 block。remote_addr=client buffer 基地址，source_offset=偏移。
  [[nodiscard]] BlockResult PutBlockUcx(
      const std::string& key,
      std::uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      std::uint64_t source_offset,
      std::uint64_t data_len);

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
