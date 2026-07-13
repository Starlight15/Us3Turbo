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
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/tcp_connection.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

/*
 * block 级写入结果。ret_code 为 PROXY_ERR_* 错误码（0=成功），调用方据此
 * 上报；error 为失败上下文（成功时为空）。crc32c/bytes_written 仅 PUT 成功时
 * 有意义（DeleteBlock 仅用 ret_code/error）。
 */
struct BlockResult {
  int           ret_code{0};
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
  std::string   error;
};

/*
 * block 级存储客户端：持有到 backend (ufile-ac) 的 TCP 连接池
 * （轮询 + 惰性重连），走自定义二进制协议（见 ufile_ac_protocol.h）。
 * 职责仅"调度 + 收发"：取连接、编码、发送、接收、解码、上报返回码；
 * 入参守卫由调用方（SinglePut / Multipart）负责。
 *
 * 并发：每连接独立 mutex（vector<unique_ptr<mutex>>，mutex 不可移动故用
 * unique_ptr）序列化请求-响应对，保证不串包；next_idx_ atomic 轮询分配。
 * 连接断开 set_dead，AcquireConn 跳过并当场重连一次，全坏返回 nullptr
 * → ret_code=PROXY_ERR_BACKEND_UNAVAILABLE。
 *
 * 日志走 LOG_SYS_*（无 rid，rid 在服务层）。
 */
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

  // 删除一个 block（尽力清理用）。ret_code/error 反映结果，调用方通常忽略
  // 失败（ufile-ac TTL 兜底）。KEY_NOT_FOUND 视为可接受的清理结果。
  [[nodiscard]] BlockResult DeleteBlock(const std::string& key);

  // GDS：读一个 block。key=block 标识，gpu_offset=写入 client GPU buffer 偏移，
  // read_offset=对象内读偏移（本阶段固定传 0），data_len=读取长度。
  // request_id=proxy 侧 rid 的 hash，透传进 backend requestId_（跨端日志关联）。
  [[nodiscard]] BlockResult GetBlockGds(
      const std::string& key,
      const std::string& rdma_token,
      std::uint64_t gpu_offset,
      std::uint64_t read_offset,
      std::uint64_t data_len,
      std::uint64_t request_id);

  // UCX：读一个 block。remote_addr=client destination buffer 基地址，
  // packed_rkey/client_ucx_addr=UCX 描述符，dest_offset=写入 destination 偏移，
  // read_offset=对象内读偏移（本阶段固定传 0），data_len=读取长度。
  // request_id=proxy 侧 rid 的 hash，透传进 backend requestId_（跨端日志关联）。
  [[nodiscard]] BlockResult GetBlockUcx(
      const std::string& key,
      std::uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      std::uint64_t dest_offset,
      std::uint64_t read_offset,
      std::uint64_t data_len,
      std::uint64_t request_id);

 private:
  // 拆分 "host:port" → host + port；失败返回 false。
  static bool ParseEndpoint(const std::string& endpoint,
                            std::string& host, int& port);
  // 方案 A 惰性取连接：轮询最多 pool_size 次，跳过/重连坏连接。
  // 返回 {idx, conn*}；全坏返回 {npos, nullptr}。
  std::pair<std::size_t, TcpConnection*> AcquireConn();

  /*
   * 通用收发骨架：取连接 → 加锁 → 发送 req_buf → 收响应头（校验 magic +
   * expected_type + body_len 下限）→ 收响应体到 out_body。
   * 成功返回 0；失败填 out_result.ret_code/error 并返回非 0。
   * 编码/解码由调用方完成（不再用 lambda）。
   */
  int SendAndRecv(const char* op_name,
                  std::uint32_t expected_type,
                  std::uint32_t min_rsp_body,
                  const std::vector<char>& req_buf,
                  std::vector<char>& out_body,
                  BlockResult& out_result);

  // 解码响应辅助（从 lambda 提取，返回填充好的 BlockResult）
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

  int      timeout_ms_;
  std::uint32_t setid_;
  std::string   host_;
  int           port_{0};
  std::vector<std::unique_ptr<TcpConnection>> conns_;
  std::vector<std::unique_ptr<std::mutex>>    conn_mutexes_;
  std::atomic<std::uint64_t> next_idx_{0};      // 轮询计数器
  std::atomic<std::uint64_t> session_seq_{0};   // sessionIdLow_ 自增（统计用）
};

}  // namespace us3_turbo::proxy
