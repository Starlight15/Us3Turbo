#include "proxy/src/storage/ufile_ac_client.h"

#include <string>
#include <utility>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/protocol_codec.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

bool UfileAcClient::ParseEndpoint(const std::string& endpoint,
                                  std::string& host, int& port) {
  const auto pos = endpoint.rfind(':');
  if (pos == std::string::npos) return false;
  host = endpoint.substr(0, pos);
  try {
    port = std::stoi(endpoint.substr(pos + 1));
  } catch (...) {
    return false;
  }
  return !host.empty() && port > 0;
}

UfileAcClient::UfileAcClient(const std::string& backend_endpoint,
                             int timeout_ms)
    : timeout_ms_(timeout_ms),
      setid_(static_cast<std::uint32_t>(FLAGS_backend_setid)) {
  if (backend_endpoint.empty()) {
    LOG_SYS_WARN("backend_endpoint empty, single-step PUT will reject as "
                 "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }
  if (!ParseEndpoint(backend_endpoint, host_, port_)) {
    LOG_SYS_WARN("backend_endpoint '{}' parse failed (expect host:port), "
                 "single-step PUT disabled", backend_endpoint);
    return;
  }

  const std::size_t pool_size =
      static_cast<std::size_t>(FLAGS_backend_conn_pool_size);
  conns_.reserve(pool_size);
  conn_mutexes_.reserve(pool_size);
  std::size_t connected = 0;
  for (std::size_t i = 0; i < pool_size; ++i) {
    auto conn = std::make_unique<TcpConnection>(host_, port_, timeout_ms_);
    if (conn->Connect()) ++connected;
    // 失败留着，AcquireConn 惰性重连（方案 A），不整体 return。
    conns_.push_back(std::move(conn));
    conn_mutexes_.push_back(std::make_unique<std::mutex>());
  }

  LOG_SYS_INFO("ufile-ac client ready (ufile-ac TCP, single-step): {} tcp "
               "connections at {}:{} (connected={}, setid={}, timeout {}ms)",
               pool_size, host_, port_, connected, setid_, timeout_ms_);
}

std::pair<std::size_t, TcpConnection*> UfileAcClient::AcquireConn() {
  const std::size_t size = conns_.size();
  if (size == 0) return {static_cast<std::size_t>(-1), nullptr};
  for (std::size_t k = 0; k < size; ++k) {
    const std::size_t idx =
        next_idx_.fetch_add(1, std::memory_order_relaxed) % size;
    TcpConnection* c = conns_[idx].get();
    if (c->alive()) return {idx, c};
    // 坏连接：在槽位锁内当场重连一次（避免多线程并发重连同一槽）
    std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);
    if (!c->alive() && c->Connect()) return {idx, c};
    // 仍坏 → 试下一槽
  }
  return {static_cast<std::size_t>(-1), nullptr};
}

// ============================ GDS PUT ============================

BlockResult UfileAcClient::PutBlockGds(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t data_len) {
  BlockResult result;

  // 1. 存储入参守卫（key 长度 / token / data_len 范围；backend 亦强制 KEY_MAX_LENGTH）
  if (key.empty() || key.size() > KEY_MAX_LENGTH) {
    result.ret_code = PROXY_ERR_INVALID_PARAM;
    result.error = "invalid key length";
    LOG_SYS_ERROR("PutBlockGds: key.size()={} exceed limit {}", key.size(), KEY_MAX_LENGTH);
    return result;
  }
  if (rdma_token.empty() || data_len == 0 || data_len > MAX_VALUE_LENGTH) {
    result.ret_code = PROXY_ERR_INVALID_PARAM;
    result.error = "invalid token or data_len";
    LOG_SYS_ERROR("PutBlockGds: token.empty={} data_len={}", rdma_token.empty(), data_len);
    return result;
  }

  // 2. 获取连接
  auto [idx, conn] = AcquireConn();
  if (conn == nullptr) {
    result.ret_code = PROXY_ERR_BACKEND_UNAVAILABLE;
    result.error = "backend pool all dead";
    LOG_SYS_WARN("PutBlockGds: no available connection");
    return result;
  }
  std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);

  // 3. 编码请求（gpu_offset 由参数传入，支持 block 偏移）
  std::vector<char> req_buf;
  codec::EncodeGdsPutRequest(
      key, rdma_token, gpu_offset, data_len, setid_,
      session_seq_.fetch_add(1, std::memory_order_relaxed), req_buf);

  LOG_SYS_DEBUG("PutBlockGds: key={} offset={} len={}", key, gpu_offset, data_len);

  // 4. 发送请求
  if (conn->SendAll(req_buf.data(), req_buf.size()) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "send request failed";
    LOG_SYS_ERROR("PutBlockGds: send failed key={}", key);
    return result;
  }

  // 5. 接收响应头（Message 52B）
  Message rmsg{};
  if (conn->RecvAll(&rmsg, MESSAGE_HEAD_SIZE) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "recv header failed";
    LOG_SYS_ERROR("PutBlockGds: recv header failed key={}", key);
    return result;
  }
  // packed 字段先拷到本地再格式化/比较（packed 字段不能绑到 fmt 的引用参数）
  const std::uint32_t rsp_magic = rmsg.magic_;
  const std::uint32_t rsp_type  = rmsg.type_;
  if (rsp_magic != MESSAGE_MAGIC_NUMBER || rsp_type != OSD_GDS_PUT_RSP) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "bad response header";
    LOG_SYS_ERROR("PutBlockGds: bad rsp magic={:#x} type={}", rsp_magic, rsp_type);
    return result;
  }
  const std::uint32_t rbody = rmsg.bodyLen_;
  if (rbody < GDS_PUT_RSP_SIZE) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "response body too short";
    LOG_SYS_ERROR("PutBlockGds: body_len={} < {}", rbody, GDS_PUT_RSP_SIZE);
    return result;
  }

  // 6. 接收响应体
  std::vector<char> rbuf(rbody);
  if (conn->RecvAll(rbuf.data(), rbody) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "recv body failed";
    LOG_SYS_ERROR("PutBlockGds: recv body failed key={}", key);
    return result;
  }

  // 7. 解码响应
  GdsPutRsp rsp{};
  std::string errmsg;
  if (codec::DecodeGdsPutResponse(rbuf.data(), rbody, rsp, errmsg) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "decode response failed: " + errmsg;
    LOG_SYS_ERROR("PutBlockGds: decode failed key={}", key);
    return result;
  }

  // 8. 检查 backend 返回码
  const std::int32_t rsp_ret = rsp.retcode_;
  if (rsp_ret != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "backend retcode=" + std::to_string(rsp_ret) + " msg=" + errmsg;
    LOG_SYS_ERROR("PutBlockGds: backend ret={} msg={} key={}", rsp_ret, errmsg, key);
    return result;
  }

  // 9. 成功
  result.ret_code      = 0;
  result.crc32c        = rsp.crc32c_;
  result.bytes_written = rsp.bytesWritten_;
  result.error.clear();
  LOG_SYS_DEBUG("PutBlockGds: ok key={} crc32c={:#x} bytes={}",
                key, result.crc32c, result.bytes_written);
  return result;
}

// ============================ UCX PUT ============================

BlockResult UfileAcClient::PutBlockUcx(
    const std::string& key,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    std::uint64_t source_offset,
    std::uint64_t data_len) {
  BlockResult result;

  // 1. 存储入参守卫
  if (key.empty() || key.size() > KEY_MAX_LENGTH) {
    result.ret_code = PROXY_ERR_INVALID_PARAM;
    result.error = "invalid key length";
    LOG_SYS_ERROR("PutBlockUcx: key.size()={} exceed limit {}", key.size(), KEY_MAX_LENGTH);
    return result;
  }
  if (remote_addr == 0 || packed_rkey.empty() || client_ucx_addr.empty() ||
      data_len == 0 || data_len > MAX_VALUE_LENGTH) {
    result.ret_code = PROXY_ERR_INVALID_PARAM;
    result.error = "invalid ucx params";
    LOG_SYS_ERROR("PutBlockUcx: remote_addr={} rkey.empty={} addr.empty={} data_len={}",
                  remote_addr, packed_rkey.empty(), client_ucx_addr.empty(), data_len);
    return result;
  }

  // 2. 获取连接
  auto [idx, conn] = AcquireConn();
  if (conn == nullptr) {
    result.ret_code = PROXY_ERR_BACKEND_UNAVAILABLE;
    result.error = "backend pool all dead";
    LOG_SYS_WARN("PutBlockUcx: no available connection");
    return result;
  }
  std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);

  // 3. 编码请求（source_offset 由参数传入，支持 block 偏移）
  std::vector<char> req_buf;
  codec::EncodeUcxPutRequest(
      key, remote_addr, packed_rkey, client_ucx_addr,
      source_offset, data_len, setid_,
      session_seq_.fetch_add(1, std::memory_order_relaxed), req_buf);

  LOG_SYS_DEBUG("PutBlockUcx: key={} offset={} len={}", key, source_offset, data_len);

  // 4. 发送请求
  if (conn->SendAll(req_buf.data(), req_buf.size()) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "send request failed";
    LOG_SYS_ERROR("PutBlockUcx: send failed key={}", key);
    return result;
  }

  // 5. 接收响应头
  Message rmsg{};
  if (conn->RecvAll(&rmsg, MESSAGE_HEAD_SIZE) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "recv header failed";
    LOG_SYS_ERROR("PutBlockUcx: recv header failed key={}", key);
    return result;
  }
  const std::uint32_t rsp_magic = rmsg.magic_;
  const std::uint32_t rsp_type  = rmsg.type_;
  if (rsp_magic != MESSAGE_MAGIC_NUMBER || rsp_type != OSD_UCX_PUT_RSP) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "bad response header";
    LOG_SYS_ERROR("PutBlockUcx: bad rsp magic={:#x} type={}", rsp_magic, rsp_type);
    return result;
  }
  const std::uint32_t rbody = rmsg.bodyLen_;
  if (rbody < UCX_PUT_RSP_SIZE) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "response body too short";
    LOG_SYS_ERROR("PutBlockUcx: body_len={} < {}", rbody, UCX_PUT_RSP_SIZE);
    return result;
  }

  // 6. 接收响应体
  std::vector<char> rbuf(rbody);
  if (conn->RecvAll(rbuf.data(), rbody) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "recv body failed";
    LOG_SYS_ERROR("PutBlockUcx: recv body failed key={}", key);
    return result;
  }

  // 7. 解码响应
  UcxPutRsp rsp{};
  std::string errmsg;
  if (codec::DecodeUcxPutResponse(rbuf.data(), rbody, rsp, errmsg) != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "decode response failed: " + errmsg;
    LOG_SYS_ERROR("PutBlockUcx: decode failed key={}", key);
    return result;
  }

  // 8. 检查 backend 返回码
  const std::int32_t rsp_ret = rsp.retcode_;
  if (rsp_ret != 0) {
    result.ret_code = PROXY_ERR_BACKEND_RPC;
    result.error = "backend retcode=" + std::to_string(rsp_ret) + " msg=" + errmsg;
    LOG_SYS_ERROR("PutBlockUcx: backend ret={} msg={} key={}", rsp_ret, errmsg, key);
    return result;
  }

  // 9. 成功
  result.ret_code      = 0;
  result.crc32c        = rsp.crc32c_;
  result.bytes_written = rsp.bytesWritten_;
  result.error.clear();
  LOG_SYS_DEBUG("PutBlockUcx: ok key={} crc32c={:#x} bytes={}",
                key, result.crc32c, result.bytes_written);
  return result;
}

}  // namespace us3_turbo::proxy
