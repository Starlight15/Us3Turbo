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
                                  std::string& host, int port) {
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

/* GDS：写一个 block。key=block 标识，gpu_offset=GPU buffer 偏移。 */
BlockResult UfileAcClient::PutBlockGds(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t data_len) {
  LOG_SYS_DEBUG("PutBlockGds: key={} offset={} len={}", key, gpu_offset, data_len);
  return DoRpc(
      "PutBlockGds", OSD_GDS_PUT_RSP, GDS_PUT_RSP_SIZE,
      [&](std::vector<char>& buf, std::uint64_t sid) {
        EncodeGdsPutRequest(key, rdma_token, gpu_offset, data_len, setid_, sid, buf);
      },
      [&](const char* body, std::uint32_t len, BlockResult& r) {
        GdsPutRsp rsp{};
        std::string errmsg;
        if (DecodeGdsPutResponse(body, len, rsp, errmsg) != 0) {
          r.ret_code = PROXY_ERR_BACKEND_RPC;
          r.error = "decode response failed: " + errmsg;
          LOG_SYS_ERROR("PutBlockGds: decode failed key={}", key);
          return;
        }
        if (rsp.retcode_ != 0) {
          const std::int32_t ret = rsp.retcode_;
          r.ret_code = PROXY_ERR_BACKEND_RPC;
          r.error = "backend retcode=" + std::to_string(ret) +
                    " msg=" + errmsg;
          LOG_SYS_ERROR("PutBlockGds: backend ret={} msg={} key={}",
                        ret, errmsg, key);
          return;
        }
        r.ret_code      = 0;
        r.crc32c        = rsp.crc32c_;
        r.bytes_written = rsp.bytesWritten_;
        LOG_SYS_DEBUG("PutBlockGds: ok key={} crc32c={:#x} bytes={}",
                      key, r.crc32c, r.bytes_written);
      });
}

// ============================ UCX PUT ============================

/* UCX：写一个 block。remote_addr=client buffer 基址，source_offset=偏移。 */
BlockResult UfileAcClient::PutBlockUcx(
    const std::string& key,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    std::uint64_t source_offset,
    std::uint64_t data_len) {
  LOG_SYS_DEBUG("PutBlockUcx: key={} offset={} len={}", key, source_offset, data_len);
  return DoRpc(
      "PutBlockUcx", OSD_UCX_PUT_RSP, UCX_PUT_RSP_SIZE,
      [&](std::vector<char>& buf, std::uint64_t sid) {
        EncodeUcxPutRequest(key, remote_addr, packed_rkey, client_ucx_addr,
                            source_offset, data_len, setid_, sid, buf);
      },
      [&](const char* body, std::uint32_t len, BlockResult& r) {
        UcxPutRsp rsp{};
        std::string errmsg;
        if (DecodeUcxPutResponse(body, len, rsp, errmsg) != 0) {
          r.ret_code = PROXY_ERR_BACKEND_RPC;
          r.error = "decode response failed: " + errmsg;
          LOG_SYS_ERROR("PutBlockUcx: decode failed key={}", key);
          return;
        }
        if (rsp.retcode_ != 0) {
          const std::int32_t ret = rsp.retcode_;
          r.ret_code = PROXY_ERR_BACKEND_RPC;
          r.error = "backend retcode=" + std::to_string(ret) +
                    " msg=" + errmsg;
          LOG_SYS_ERROR("PutBlockUcx: backend ret={} msg={} key={}",
                        ret, errmsg, key);
          return;
        }
        r.ret_code      = 0;
        r.crc32c        = rsp.crc32c_;
        r.bytes_written = rsp.bytesWritten_;
        LOG_SYS_DEBUG("PutBlockUcx: ok key={} crc32c={:#x} bytes={}",
                      key, r.crc32c, r.bytes_written);
      });
}

// ============================ DEL（尽力清理）============================

/* 删除一个 block。清理场景 retcode!=0 按 WARN 处理（KEY_NOT_FOUND 可接受）。 */
BlockResult UfileAcClient::DeleteBlock(const std::string& key) {
  LOG_SYS_DEBUG("DeleteBlock: key={}", key);
  return DoRpc(
      "DeleteBlock", OSD_DEL_RSP, DEL_RSP_SIZE,
      [&](std::vector<char>& buf, std::uint64_t sid) {
        EncodeDelRequest(key, setid_, sid, buf);
      },
      [&](const char* body, std::uint32_t len, BlockResult& r) {
        DelRsp rsp{};
        std::string errmsg;
        if (DecodeDelResponse(body, len, rsp, errmsg) != 0) {
          r.ret_code = PROXY_ERR_BACKEND_RPC;
          r.error = "decode response failed: " + errmsg;
          LOG_SYS_ERROR("DeleteBlock: decode failed key={}", key);
          return;
        }
        if (rsp.retcode_ != 0) {
          const std::int32_t ret = rsp.retcode_;
          r.ret_code = PROXY_ERR_BACKEND_RPC;
          r.error = "backend retcode=" + std::to_string(ret) +
                    " msg=" + errmsg;
          // 清理容错：非 0（含 KEY_NOT_FOUND）按 WARN，不当作致命错误
          LOG_SYS_WARN("DeleteBlock: backend ret={} msg={} key={}",
                       ret, errmsg, key);
          return;
        }
        r.ret_code = 0;
        LOG_SYS_DEBUG("DeleteBlock: ok key={}", key);
      });
}

}  // namespace us3_turbo::proxy
