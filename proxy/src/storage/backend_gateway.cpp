#include "proxy/src/storage/backend_gateway.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy {

// ============================ TcpConnection ============================

TcpConnection::TcpConnection(std::string host, int port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), fd_(-1) {}

TcpConnection::~TcpConnection() { Close(); }

bool TcpConnection::Connect() {
  if (alive_.load(std::memory_order_acquire)) return true;
  if (fd_ >= 0) Close();  // 清理残留 fd

  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) return false;

  struct timeval tv;
  tv.tv_sec  = timeout_ms_ / 1000;
  tv.tv_usec = (timeout_ms_ % 1000) * 1000;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(static_cast<std::uint16_t>(port_));
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
    Close();
    return false;
  }
  if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    Close();
    return false;
  }
  alive_.store(true, std::memory_order_release);
  return true;
}

void TcpConnection::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  alive_.store(false, std::memory_order_release);
}

int TcpConnection::SendAll(const void* buf, std::size_t len) {
  if (!alive_.load(std::memory_order_acquire)) return -1;  // 须先经 AcquireConn->Connect
  std::size_t sent = 0;
  const auto* p = static_cast<const char*>(buf);
  while (sent < len) {
    ssize_t n = ::send(fd_, p + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      set_dead();
      Close();
      return -1;
    }
    sent += static_cast<std::size_t>(n);
  }
  return 0;
}

int TcpConnection::RecvAll(void* buf, std::size_t len) {
  if (!alive_.load(std::memory_order_acquire)) return -1;
  std::size_t got = 0;
  auto* p = static_cast<char*>(buf);
  while (got < len) {
    ssize_t n = ::recv(fd_, p + got, len - got, MSG_WAITALL);
    if (n < 0) {
      if (errno == EINTR) continue;
      set_dead();
      Close();
      return -1;
    }
    if (n == 0) {  // 对端关闭
      set_dead();
      Close();
      return -1;
    }
    got += static_cast<std::size_t>(n);
  }
  return 0;
}

// ============================ BackendGateway ============================

bool BackendGateway::ParseEndpoint(const std::string& endpoint,
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

BackendGateway::BackendGateway(const std::string& backend_endpoint,
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

  LOG_SYS_INFO("backend gateway ready (ufile-ac TCP, single-step): {} tcp "
               "connections at {}:{} (connected={}, setid={}, timeout {}ms)",
               pool_size, host_, port_, connected, setid_, timeout_ms_);
}

std::pair<std::size_t, TcpConnection*> BackendGateway::AcquireConn() {
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

int BackendGateway::ForwardGdsPut(
    const ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  auto [idx, conn] = AcquireConn();
  if (conn == nullptr) {
    LOG_WARN(rid, "backend pool all dead");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }
  std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);

  const std::string& key   = request.key();
  const std::string& token = request.gds_source().rdma_token();
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(token.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(GDS_PUT_REQ_SIZE + key_len + tok_len);
  const std::uint32_t msg_size_field =
      static_cast<std::uint32_t>(MESSAGE_HEAD_SIZE + body_len -
                                 sizeof(std::uint32_t));

  // 请求头（仅 msgSize_ 大端，其余主机序直接赋值，F2）
  Message msg{};
  msg.msgSize_      = htonl(msg_size_field);
  msg.magic_        = MESSAGE_MAGIC_NUMBER;
  msg.version_      = MESSAGE_VERSION_NUMBER;
  msg.type_         = OSD_GDS_PUT_REQ;
  msg.sessionIdLow_ = session_seq_.fetch_add(1, std::memory_order_relaxed);  // 仅统计
  msg.setid_        = setid_;
  msg.bodyLen_      = body_len;

  // GdsPutReq（requestId_/sessionId*_/flags_ 预留填 0，backend 不读，F4）
  GdsPutReq req{};
  req.keyLen_    = key_len;
  req.tokenLen_  = tok_len;
  req.dataLen_   = request.object_size();
  req.gpuOffset_ = 0;

  std::string buf;
  buf.reserve(MESSAGE_HEAD_SIZE + body_len);
  buf.append(reinterpret_cast<const char*>(&msg), MESSAGE_HEAD_SIZE);
  buf.append(reinterpret_cast<const char*>(&req), GDS_PUT_REQ_SIZE);
  buf.append(key);
  buf.append(token);

  LOG_DEBUG(rid, "sending GdsPut bucket={}/{} size={} klen={} tlen={}",
            request.bucket(), request.key(), request.object_size(),
            key_len, tok_len);

  if (conn->SendAll(buf.data(), buf.size()) != 0) {
    LOG_ERROR(rid, "send GdsPutReq failed");
    return PROXY_ERR_BACKEND_RPC;
  }

  Message rmsg{};
  if (conn->RecvAll(&rmsg, MESSAGE_HEAD_SIZE) != 0) {
    LOG_ERROR(rid, "recv GdsPut rsp header failed");
    return PROXY_ERR_BACKEND_RPC;
  }
  // packed 字段先拷到本地再格式化/比较（packed 字段不能绑到 fmt 的引用参数）
  const std::uint32_t rsp_magic = rmsg.magic_;
  const std::uint32_t rsp_type  = rmsg.type_;
  if (rsp_magic != MESSAGE_MAGIC_NUMBER || rsp_type != OSD_GDS_PUT_RSP) {
    LOG_ERROR(rid, "bad GdsPut rsp magic={:x} type={}", rsp_magic, rsp_type);
    return PROXY_ERR_BACKEND_RPC;
  }
  const std::uint32_t rbody = rmsg.bodyLen_;
  if (rbody < GDS_PUT_RSP_SIZE) {
    LOG_ERROR(rid, "GdsPut rsp body too short {}", rbody);
    return PROXY_ERR_BACKEND_RPC;
  }
  std::vector<char> rbuf(rbody);
  if (conn->RecvAll(rbuf.data(), rbody) != 0) {
    LOG_ERROR(rid, "recv GdsPut rsp body failed");
    return PROXY_ERR_BACKEND_RPC;
  }

  GdsPutRsp rsp{};
  std::memcpy(&rsp, rbuf.data(), GDS_PUT_RSP_SIZE);
  const std::uint32_t rsp_etag   = rsp.etagLen_;
  const std::uint32_t rsp_errmsg = rsp.errMsgLen_;
  const std::int32_t  rsp_ret    = rsp.retcode_;
  const std::uint32_t var_len =
      static_cast<std::uint32_t>(rbody - GDS_PUT_RSP_SIZE);
  if (rsp_etag + rsp_errmsg > var_len) {
    LOG_ERROR(rid, "GdsPut rsp var overflow etag={} errmsg={} var={}",
              rsp_etag, rsp_errmsg, var_len);
    return PROXY_ERR_BACKEND_RPC;
  }
  std::string errmsg(rbuf.data() + GDS_PUT_RSP_SIZE + rsp_etag, rsp_errmsg);

  if (rsp_ret != 0) {
    LOG_ERROR(rid, "backend GdsPut retcode={} msg={}", rsp_ret, errmsg);
    return PROXY_ERR_BACKEND_RPC;
  }

  out.etag.assign(rbuf.data() + GDS_PUT_RSP_SIZE, rsp_etag);  // etagLen_=0 → 空（F7）
  out.crc32c        = rsp.crc32c_;
  out.bytes_written = rsp.bytesWritten_;
  LOG_DEBUG(rid, "backend GdsPut ok crc32c={:#x} bytes={}",
            out.crc32c, out.bytes_written);
  return 0;
}

int BackendGateway::ForwardUcxPut(
    const ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  auto [idx, conn] = AcquireConn();
  if (conn == nullptr) {
    LOG_WARN(rid, "backend pool all dead");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }
  std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);

  const std::string& key = request.key();
  const auto& src = request.ucx_source();
  const std::string& ucx_addr   = src.client_ucx_addr();
  const std::string& packed_rkey = src.packed_rkey();
  const std::uint32_t key_len  = static_cast<std::uint32_t>(key.size());
  const std::uint32_t addr_len = static_cast<std::uint32_t>(ucx_addr.size());
  const std::uint32_t rkey_len = static_cast<std::uint32_t>(packed_rkey.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(UCX_PUT_REQ_SIZE + key_len + addr_len + rkey_len);
  const std::uint32_t msg_size_field =
      static_cast<std::uint32_t>(MESSAGE_HEAD_SIZE + body_len -
                                 sizeof(std::uint32_t));

  Message msg{};
  msg.msgSize_      = htonl(msg_size_field);
  msg.magic_        = MESSAGE_MAGIC_NUMBER;
  msg.version_      = MESSAGE_VERSION_NUMBER;
  msg.type_         = OSD_UCX_PUT_REQ;
  msg.sessionIdLow_ = session_seq_.fetch_add(1, std::memory_order_relaxed);
  msg.setid_        = setid_;
  msg.bodyLen_      = body_len;

  // UcxPutReq（reserved0_/requestId_/sessionId*_/flags_ 预留填 0）
  UcxPutReq req{};
  req.keyLen_       = key_len;
  req.addrLen_      = addr_len;
  req.rkeyLen_      = rkey_len;
  req.dataLen_      = request.object_size();
  req.remoteAddr_   = src.remote_addr();
  req.sourceOffset_ = 0;

  std::string buf;
  buf.reserve(MESSAGE_HEAD_SIZE + body_len);
  buf.append(reinterpret_cast<const char*>(&msg), MESSAGE_HEAD_SIZE);
  buf.append(reinterpret_cast<const char*>(&req), UCX_PUT_REQ_SIZE);
  buf.append(key);
  buf.append(ucx_addr);
  buf.append(packed_rkey);

  LOG_DEBUG(rid, "sending UcxPut bucket={}/{} size={} klen={} alen={} rlen={}",
            request.bucket(), request.key(), request.object_size(),
            key_len, addr_len, rkey_len);

  if (conn->SendAll(buf.data(), buf.size()) != 0) {
    LOG_ERROR(rid, "send UcxPutReq failed");
    return PROXY_ERR_BACKEND_RPC;
  }

  Message rmsg{};
  if (conn->RecvAll(&rmsg, MESSAGE_HEAD_SIZE) != 0) {
    LOG_ERROR(rid, "recv UcxPut rsp header failed");
    return PROXY_ERR_BACKEND_RPC;
  }
  const std::uint32_t rsp_magic = rmsg.magic_;
  const std::uint32_t rsp_type  = rmsg.type_;
  if (rsp_magic != MESSAGE_MAGIC_NUMBER || rsp_type != OSD_UCX_PUT_RSP) {
    LOG_ERROR(rid, "bad UcxPut rsp magic={:x} type={}", rsp_magic, rsp_type);
    return PROXY_ERR_BACKEND_RPC;
  }
  const std::uint32_t rbody = rmsg.bodyLen_;
  if (rbody < UCX_PUT_RSP_SIZE) {
    LOG_ERROR(rid, "UcxPut rsp body too short {}", rbody);
    return PROXY_ERR_BACKEND_RPC;
  }
  std::vector<char> rbuf(rbody);
  if (conn->RecvAll(rbuf.data(), rbody) != 0) {
    LOG_ERROR(rid, "recv UcxPut rsp body failed");
    return PROXY_ERR_BACKEND_RPC;
  }

  UcxPutRsp rsp{};
  std::memcpy(&rsp, rbuf.data(), UCX_PUT_RSP_SIZE);
  const std::uint32_t rsp_errmsg = rsp.errMsgLen_;
  const std::int32_t  rsp_ret    = rsp.retcode_;
  const std::uint32_t var_len =
      static_cast<std::uint32_t>(rbody - UCX_PUT_RSP_SIZE);
  if (rsp_errmsg > var_len) {
    LOG_ERROR(rid, "UcxPut rsp var overflow errmsg={} var={}", rsp_errmsg, var_len);
    return PROXY_ERR_BACKEND_RPC;
  }
  std::string errmsg(rbuf.data() + UCX_PUT_RSP_SIZE, rsp_errmsg);

  if (rsp_ret != 0) {
    LOG_ERROR(rid, "backend UcxPut retcode={} msg={}", rsp_ret, errmsg);
    return PROXY_ERR_BACKEND_RPC;
  }

  out.etag.clear();  // UcxPutRsp 无 etag 字段
  out.crc32c        = rsp.crc32c_;
  out.bytes_written = rsp.bytesWritten_;
  LOG_DEBUG(rid, "backend UcxPut ok crc32c={:#x} bytes={}",
            out.crc32c, out.bytes_written);
  return 0;
}

}  // namespace us3_turbo::proxy
