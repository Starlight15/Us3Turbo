#include "proxy/src/storage/ufile_ac_client.h"

#include <string>
#include <utility>
#include <vector>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

/* 拆分 "host:port" → host + port；失败返回 false。 */
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

/*
 * 构造连接池：逐连接 Connect，失败留着 AcquireConn 惰性重连（方案 A），
 * 不整体 return。pool 就绪后打一条汇总日志。
 */
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
    conns_.push_back(std::move(conn));
    conn_mutexes_.push_back(std::make_unique<std::mutex>());
  }

  LOG_SYS_INFO("ufile-ac client ready (ufile-ac TCP, single-step): {} tcp "
               "connections at {}:{} (connected={}, setid={}, timeout {}ms)",
               pool_size, host_, port_, connected, setid_, timeout_ms_);
}

/*
 * 方案 A 惰性取连接：轮询最多 pool_size 次，跳过坏连接；坏连接在槽位锁内
 * 当场重连一次（避免多线程并发重连同一槽）。返回 {idx, conn*}；全坏 {npos,null}。
 */
std::pair<std::size_t, TcpConnection*> UfileAcClient::AcquireConn() {
  const std::size_t size = conns_.size();
  if (size == 0) return {static_cast<std::size_t>(-1), nullptr};
  for (std::size_t k = 0; k < size; ++k) {
    const std::size_t idx =
        next_idx_.fetch_add(1, std::memory_order_relaxed) % size;
    TcpConnection* c = conns_[idx].get();
    if (c->alive()) return {idx, c};
    std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);
    if (!c->alive() && c->Connect()) return {idx, c};
  }
  return {static_cast<std::size_t>(-1), nullptr};
}

// ============================ GDS PUT ============================

BlockResult UfileAcClient::PutBlockGds(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t data_len) {
  LOG_SYS_DEBUG("PutBlockGds: key={} offset={} len={}", key, gpu_offset, data_len);

  // 编码
  std::vector<char> req;
  EncodeGdsPutRequest(key, rdma_token, gpu_offset, data_len, setid_,
                      session_seq_.fetch_add(1, std::memory_order_relaxed), req);

  // 收发
  std::vector<char> rsp_body;
  BlockResult result;
  if (SendAndRecv("PutBlockGds", OSD_GDS_PUT_RSP, GDS_PUT_RSP_SIZE,
                  req, rsp_body, result) != 0) {
    return result;  // 已填错误
  }

  // 解码
  return DecodeGdsPutRsp(rsp_body.data(),
                         static_cast<std::uint32_t>(rsp_body.size()), key);
}

// ============================ UCX PUT ============================

BlockResult UfileAcClient::PutBlockUcx(
    const std::string& key,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    std::uint64_t source_offset,
    std::uint64_t data_len) {
  LOG_SYS_DEBUG("PutBlockUcx: key={} offset={} len={}", key, source_offset, data_len);

  // 编码
  std::vector<char> req;
  EncodeUcxPutRequest(key, remote_addr, packed_rkey, client_ucx_addr,
                      source_offset, data_len, setid_,
                      session_seq_.fetch_add(1, std::memory_order_relaxed), req);

  // 收发
  std::vector<char> rsp_body;
  BlockResult result;
  if (SendAndRecv("PutBlockUcx", OSD_UCX_PUT_RSP, UCX_PUT_RSP_SIZE,
                  req, rsp_body, result) != 0) {
    return result;
  }

  // 解码
  return DecodeUcxPutRsp(rsp_body.data(),
                         static_cast<std::uint32_t>(rsp_body.size()), key);
}

// ============================ DEL（尽力清理）============================

BlockResult UfileAcClient::DeleteBlock(const std::string& key) {
  LOG_SYS_DEBUG("DeleteBlock: key={}", key);

  // 编码
  std::vector<char> req;
  EncodeDelRequest(key, setid_,
                   session_seq_.fetch_add(1, std::memory_order_relaxed), req);

  // 收发
  std::vector<char> rsp_body;
  BlockResult result;
  if (SendAndRecv("DeleteBlock", OSD_DEL_RSP, DEL_RSP_SIZE,
                  req, rsp_body, result) != 0) {
    return result;
  }

  // 解码
  return DecodeDelRsp(rsp_body.data(),
                      static_cast<std::uint32_t>(rsp_body.size()), key);
}

// ============================ 通用收发骨架 ============================

/*
 * 通用收发骨架：取连接 → 加锁 → 发送 req_buf → 收响应头（校验）→ 收响应体。
 * 成功返回 0，out_body 填充响应体；失败返回非 0，out_result 填 error。
 */
int UfileAcClient::SendAndRecv(const char* op_name,
                               std::uint32_t expected_type,
                               std::uint32_t min_rsp_body,
                               const std::vector<char>& req_buf,
                               std::vector<char>& out_body,
                               BlockResult& out_result) {
  // 取连接
  auto [idx, conn] = AcquireConn();
  if (conn == nullptr) {
    out_result.ret_code = PROXY_ERR_BACKEND_UNAVAILABLE;
    out_result.error = "backend pool all dead";
    LOG_SYS_WARN("{}: no available connection", op_name);
    return -1;
  }
  std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);

  // 发送
  if (conn->SendAll(req_buf.data(), req_buf.size()) != 0) {
    out_result.ret_code = PROXY_ERR_BACKEND_RPC;
    out_result.error = "send request failed";
    LOG_SYS_ERROR("{}: send failed", op_name);
    return -1;
  }

  // 收响应头并校验
  Message rmsg{};
  if (conn->RecvAll(&rmsg, MESSAGE_HEAD_SIZE) != 0) {
    out_result.ret_code = PROXY_ERR_BACKEND_RPC;
    out_result.error = "recv header failed";
    LOG_SYS_ERROR("{}: recv header failed", op_name);
    return -1;
  }
  const std::uint32_t rsp_magic = rmsg.magic_;
  const std::uint32_t rsp_type  = rmsg.type_;
  if (rsp_magic != MESSAGE_MAGIC_NUMBER || rsp_type != expected_type) {
    out_result.ret_code = PROXY_ERR_BACKEND_RPC;
    out_result.error = "bad response header";
    LOG_SYS_ERROR("{}: bad rsp magic={:#x} type={}", op_name, rsp_magic, rsp_type);
    return -1;
  }
  const std::uint32_t rbody = rmsg.bodyLen_;
  if (rbody < min_rsp_body) {
    out_result.ret_code = PROXY_ERR_BACKEND_RPC;
    out_result.error = "response body too short";
    LOG_SYS_ERROR("{}: body_len={} < {}", op_name, rbody, min_rsp_body);
    return -1;
  }

  // 收响应体
  out_body.resize(rbody);
  if (conn->RecvAll(out_body.data(), rbody) != 0) {
    out_result.ret_code = PROXY_ERR_BACKEND_RPC;
    out_result.error = "recv body failed";
    LOG_SYS_ERROR("{}: recv body failed", op_name);
    return -1;
  }

  return 0;
}

// ============================ 解码辅助（从 lambda 提取）============================

BlockResult UfileAcClient::DecodeGdsPutRsp(const char* body,
                                           std::uint32_t body_len,
                                           const std::string& key) {
  GdsPutRsp rsp{};
  std::string errmsg;
  if (DecodeGdsPutResponse(body, body_len, rsp, errmsg) != 0) {
    LOG_SYS_ERROR("PutBlockGds: decode failed key={}", key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "decode response failed: " + errmsg;
    return r;
  }
  if (rsp.retcode_ != 0) {
    const std::int32_t ret = rsp.retcode_;
    LOG_SYS_ERROR("PutBlockGds: backend ret={} msg={} key={}", ret, errmsg, key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "backend retcode=" + std::to_string(ret) + " msg=" + errmsg;
    return r;
  }
  // packed 字段先拷贝到临时变量，避免绑定引用错误
  const std::uint32_t crc = rsp.crc32c_;
  const std::uint64_t written = rsp.bytesWritten_;
  LOG_SYS_DEBUG("PutBlockGds: ok key={} crc32c={:#x} bytes={}", key, crc, written);
  BlockResult r;
  r.ret_code = 0;
  r.crc32c = crc;
  r.bytes_written = written;
  return r;
}

BlockResult UfileAcClient::DecodeUcxPutRsp(const char* body,
                                           std::uint32_t body_len,
                                           const std::string& key) {
  UcxPutRsp rsp{};
  std::string errmsg;
  if (DecodeUcxPutResponse(body, body_len, rsp, errmsg) != 0) {
    LOG_SYS_ERROR("PutBlockUcx: decode failed key={}", key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "decode response failed: " + errmsg;
    return r;
  }
  if (rsp.retcode_ != 0) {
    const std::int32_t ret = rsp.retcode_;
    LOG_SYS_ERROR("PutBlockUcx: backend ret={} msg={} key={}", ret, errmsg, key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "backend retcode=" + std::to_string(ret) + " msg=" + errmsg;
    return r;
  }
  // packed 字段先拷贝到临时变量，避免绑定引用错误
  const std::uint32_t crc = rsp.crc32c_;
  const std::uint64_t written = rsp.bytesWritten_;
  LOG_SYS_DEBUG("PutBlockUcx: ok key={} crc32c={:#x} bytes={}", key, crc, written);
  BlockResult r;
  r.ret_code = 0;
  r.crc32c = crc;
  r.bytes_written = written;
  return r;
}

BlockResult UfileAcClient::DecodeDelRsp(const char* body,
                                        std::uint32_t body_len,
                                        const std::string& key) {
  DelRsp rsp{};
  std::string errmsg;
  if (DecodeDelResponse(body, body_len, rsp, errmsg) != 0) {
    LOG_SYS_ERROR("DeleteBlock: decode failed key={}", key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "decode response failed: " + errmsg;
    return r;
  }
  if (rsp.retcode_ != 0) {
    const std::int32_t ret = rsp.retcode_;
    // 清理容错：非 0（含 KEY_NOT_FOUND）按 WARN，不当作致命错误
    LOG_SYS_WARN("DeleteBlock: backend ret={} msg={} key={}", ret, errmsg, key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "backend retcode=" + std::to_string(ret) + " msg=" + errmsg;
    return r;
  }
  LOG_SYS_DEBUG("DeleteBlock: ok key={}", key);
  return BlockResult{};  // 默认全 0，成功
}

// ============================ GDS GET ============================

BlockResult UfileAcClient::GetBlockGds(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t read_offset,
    std::uint64_t data_len) {
  LOG_SYS_DEBUG("GetBlockGds: key={} gpu_offset={} read_offset={} len={}",
                key, gpu_offset, read_offset, data_len);

  // 编码
  std::vector<char> req;
  EncodeGdsGetRequest(key, rdma_token, read_offset, gpu_offset, data_len, setid_,
                      session_seq_.fetch_add(1, std::memory_order_relaxed), req);

  // 收发
  std::vector<char> rsp_body;
  BlockResult result;
  if (SendAndRecv("GetBlockGds", OSD_GDS_GET_RSP, GDS_GET_RSP_SIZE,
                  req, rsp_body, result) != 0) {
    return result;  // 已填错误
  }

  // 解码
  return DecodeGdsGetRsp(rsp_body.data(),
                         static_cast<std::uint32_t>(rsp_body.size()), key);
}

BlockResult UfileAcClient::DecodeGdsGetRsp(const char* body,
                                           std::uint32_t body_len,
                                           const std::string& key) {
  GdsGetRsp rsp{};
  std::string errmsg;
  if (DecodeGdsGetResponse(body, body_len, rsp, errmsg) != 0) {
    LOG_SYS_ERROR("GetBlockGds: decode failed key={}", key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "decode response failed: " + errmsg;
    return r;
  }
  if (rsp.retcode_ != 0) {
    const std::int32_t ret = rsp.retcode_;
    LOG_SYS_ERROR("GetBlockGds: backend ret={} msg={} key={}", ret, errmsg, key);
    BlockResult r;
    r.ret_code = PROXY_ERR_BACKEND_RPC;
    r.error = "backend retcode=" + std::to_string(ret) + " msg=" + errmsg;
    return r;
  }
  // packed 字段先拷贝到临时变量，避免绑定引用错误
  const std::uint32_t crc = rsp.crc32c_;
  const std::uint64_t read = rsp.bytesRead_;
  LOG_SYS_DEBUG("GetBlockGds: ok key={} crc32c={:#x} bytes={}", key, crc, read);
  BlockResult r;
  r.ret_code = 0;
  r.crc32c = crc;
  r.bytes_written = read;  // GET 语义下复用字段 = bytes_read
  return r;
}

}  // namespace us3_turbo::proxy
