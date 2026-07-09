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

 private:
  // 拆分 "host:port" → host + port；失败返回 false。
  static bool ParseEndpoint(const std::string& endpoint,
                            std::string& host, int port);
  // 方案 A 惰性取连接：轮询最多 pool_size 次，跳过/重连坏连接。
  // 返回 {idx, conn*}；全坏返回 {npos, nullptr}。
  std::pair<std::size_t, TcpConnection*> AcquireConn();

  /*
   * 通用 RPC 骨架：取连接 → 加锁 → encode_fn 编码 → 发送
   *   → 收响应头（校验 magic + expected_type + body_len 下限）
   *   → 收响应体 → decode_fn 解码并填 result。
   * encode_fn: (std::vector<char>& out_buf, std::uint64_t session_id) -> void
   * decode_fn: (const char* body, std::uint32_t body_len, BlockResult& r) -> void
   * op_name 仅用于日志。任一环节失败填 result.ret_code/error 并返回。
   */
  template <typename EncodeFn, typename DecodeFn>
  BlockResult DoRpc(const char* op_name,
                    std::uint32_t expected_type,
                    std::uint32_t min_rsp_body,
                    EncodeFn encode_fn,
                    DecodeFn decode_fn);

  int      timeout_ms_;
  std::uint32_t setid_;
  std::string   host_;
  int           port_{0};
  std::vector<std::unique_ptr<TcpConnection>> conns_;
  std::vector<std::unique_ptr<std::mutex>>    conn_mutexes_;
  std::atomic<std::uint64_t> next_idx_{0};      // 轮询计数器
  std::atomic<std::uint64_t> session_seq_{0};   // sessionIdLow_ 自增（统计用）
};

// ============================ DoRpc 模板实现 ============================

template <typename EncodeFn, typename DecodeFn>
BlockResult UfileAcClient::DoRpc(const char* op_name,
                                 std::uint32_t expected_type,
                                 std::uint32_t min_rsp_body,
                                 EncodeFn encode_fn,
                                 DecodeFn decode_fn) {
  BlockResult result;

  // 取连接（入参守卫已在调用方完成）
  auto [idx, conn] = AcquireConn();
  if (conn == nullptr) {
    result.ret_code = PROXY_ERR_BACKEND_UNAVAILABLE;
    result.error = "backend pool all dead";
    LOG_SYS_WARN("{}: no available connection", op_name);
    return result;
  }
  std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);

  // 编码 + 发送
  std::vector<char> req_buf;
  encode_fn(req_buf, session_seq_.fetch_add(1, std::memory_order_relaxed));
  if (conn->SendAll(req_buf.data(), req_buf.size()) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "send request failed";
    LOG_SYS_ERROR("{}: send failed", op_name);
    return result;
  }

  // 收响应头并校验 magic / type
  Message rmsg{};
  if (conn->RecvAll(&rmsg, MESSAGE_HEAD_SIZE) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "recv header failed";
    LOG_SYS_ERROR("{}: recv header failed", op_name);
    return result;
  }
  // packed 字段不能直接绑 fmt 引用，先拷本地
  const std::uint32_t rsp_magic = rmsg.magic_;
  const std::uint32_t rsp_type  = rmsg.type_;
  if (rsp_magic != MESSAGE_MAGIC_NUMBER || rsp_type != expected_type) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "bad response header";
    LOG_SYS_ERROR("{}: bad rsp magic={:#x} type={}", op_name, rsp_magic, rsp_type);
    return result;
  }
  const std::uint32_t rbody = rmsg.bodyLen_;
  if (rbody < min_rsp_body) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "response body too short";
    LOG_SYS_ERROR("{}: body_len={} < {}", op_name, rbody, min_rsp_body);
    return result;
  }

  // 收响应体
  std::vector<char> rbuf(rbody);
  if (conn->RecvAll(rbuf.data(), rbody) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "recv body failed";
    LOG_SYS_ERROR("{}: recv body failed", op_name);
    return result;
  }

  // 解码 + 填 result（含 retcode 校验），由调用方 lambda 完成
  decode_fn(rbuf.data(), rbody, result);
  return result;
}

}  // namespace us3_turbo::proxy
