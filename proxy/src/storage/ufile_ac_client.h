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
#include "proxy/src/common/flags.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/tcp_connection.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

/* block 级写入结果。ret_code 为 PROXY_ERR_* 错误码（0=成功）。
 * crc32c/bytes_written 仅 PUT 成功时有效；DeleteBlock 仅用 ret_code/error。 */
struct BlockResult {
  int ret_code{0};
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
  std::string error;
};

/* block 级存储客户端：持有 TCP 连接池（轮询+惰性重连），走自定义二进制协议。
 * 职责仅"调度+收发"；入参守卫由调用方负责。日志走 LOG_SYS_*（无 rid）。
 * 并发：每连接独立 mutex 序列化请求-响应对，轮询分配；全坏返回 UNAVAILABLE。 */
class UfileAcClient {
 public:
  UfileAcClient(const std::string& backend_endpoint, int timeout_ms)
      : timeout_ms_(timeout_ms), setid_(static_cast<std::uint32_t>(FLAGS_backend_setid)) {
    if (backend_endpoint.empty()) {
      LOG_SYS_WARN(
          "backend_endpoint empty, single-step PUT will reject as "
          "PROXY_ERR_BACKEND_UNAVAILABLE");
      return;
    }
    if (!ParseEndpoint(backend_endpoint, host_, port_)) {
      LOG_SYS_WARN(
          "backend_endpoint '{}' parse failed (expect host:port), "
          "single-step PUT disabled",
          backend_endpoint);
      return;
    }

    const std::size_t pool_size = static_cast<std::size_t>(FLAGS_backend_conn_pool_size);
    conns_.reserve(pool_size);
    conn_mutexes_.reserve(pool_size);
    std::size_t connected = 0;
    for (std::size_t i = 0; i < pool_size; ++i) {
      auto conn = std::make_unique<TcpConnection>(host_, port_, timeout_ms_);
      if (conn->Connect()) ++connected;
      conns_.push_back(std::move(conn));
      conn_mutexes_.push_back(std::make_unique<std::mutex>());
    }

    LOG_SYS_INFO(
        "ufile-ac client ready (ufile-ac TCP, single-step): {} tcp "
        "connections at {}:{} (connected={}, setid={}, timeout {}ms)",
        pool_size, host_, port_, connected, setid_, timeout_ms_);
  }

  /* GDS 写一个 block 到 backend。 */
  [[nodiscard]] BlockResult PutBlockGds(const std::string& key,
                                        const std::string& rdma_token,
                                        std::uint64_t gpu_offset, std::uint64_t data_len);

  /* UCX 写一个 block 到 backend。 */
  [[nodiscard]] BlockResult PutBlockUcx(const std::string& key, std::uint64_t remote_addr,
                                        const std::string& packed_rkey,
                                        const std::string& client_ucx_addr,
                                        std::uint64_t source_offset,
                                        std::uint64_t data_len);

  /* 尽力删除一个 block；失败通常忽略（TTL 兜底），KEY_NOT_FOUND 视为可接受。 */
  [[nodiscard]] BlockResult DeleteBlock(const std::string& key);

  /* GDS 读一个 block 到 client GPU buffer。request_id 透传进 backend
   * 供日志关联。 */
  [[nodiscard]] BlockResult GetBlockGds(const std::string& key,
                                        const std::string& rdma_token,
                                        std::uint64_t gpu_offset,
                                        std::uint64_t read_offset, std::uint64_t data_len,
                                        std::uint64_t request_id);

  /* UCX 读一个 block 到 client buffer。request_id 透传进 backend 供日志关联。
   */
  [[nodiscard]] BlockResult GetBlockUcx(const std::string& key, std::uint64_t remote_addr,
                                        const std::string& packed_rkey,
                                        const std::string& client_ucx_addr,
                                        std::uint64_t dest_offset,
                                        std::uint64_t read_offset, std::uint64_t data_len,
                                        std::uint64_t request_id);

 private:
  /* 拆分 "host:port" 为 host + port；失败返回 false。 */
  static bool ParseEndpoint(const std::string& endpoint, std::string& host, int& port);
  /* 惰性取连接：轮询跳过/重连坏连接，返回 {idx, conn*}；全坏返回 {npos,
   * nullptr}。 */
  std::pair<std::size_t, TcpConnection*> AcquireConn();

  /* 通用收发骨架：取连接→加锁→发送→收响应头(校验magic/type/body_len)→收响应体。
   * 成功返回0；失败填out_result并返回非0。编码/解码由调用方完成。 */
  int SendAndRecv(const char* op_name, std::uint32_t expected_type,
                  std::uint32_t min_rsp_body, const std::vector<char>& req_buf,
                  std::vector<char>& out_body, BlockResult& out_result);

  /* 解码响应辅助，返回填充好的 BlockResult。 */
  static BlockResult DecodeGdsPutRsp(const char* body, std::uint32_t body_len,
                                     const std::string& key);
  static BlockResult DecodeUcxPutRsp(const char* body, std::uint32_t body_len,
                                     const std::string& key);
  static BlockResult DecodeDelRsp(const char* body, std::uint32_t body_len,
                                  const std::string& key);
  static BlockResult DecodeGdsGetRsp(const char* body, std::uint32_t body_len,
                                     const std::string& key);
  static BlockResult DecodeUcxGetRsp(const char* body, std::uint32_t body_len,
                                     const std::string& key);

  int timeout_ms_;
  std::uint32_t setid_;
  std::string host_;
  int port_{0};
  std::vector<std::unique_ptr<TcpConnection>> conns_;
  std::vector<std::unique_ptr<std::mutex>> conn_mutexes_;
  std::atomic<std::uint64_t> next_idx_{0};     // 轮询计数器
  std::atomic<std::uint64_t> session_seq_{0};  // sessionIdLow_ 自增（统计用）
};

}  // namespace us3_turbo::proxy
