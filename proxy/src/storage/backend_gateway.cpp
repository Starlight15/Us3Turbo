#include "proxy/src/storage/backend_gateway.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/logging/logger.h"
#include "proxy/src/storage/backend_protocol.h"

namespace us3_turbo::proxy {

namespace {

// ufile-ac backend 对 key 长度的硬限制（message.h KEY_MAX_LENGTH）。
constexpr std::uint32_t kKeyMaxLen = 48;

// ufile-ac backend 对 UCX 请求 rkey 长度的硬限制（ac_server.cc OSD_UCX_PUT_REQ 分支）。
constexpr std::uint32_t kRkeyMaxLen = 65536;

// backend 不返回 etag（GdsPutRsp.etagLen_ 恒为 0），proxy 用返回的 crc32c 合成
// 一个确定性 etag 代理值（同内容 → 同 crc32c → 同 etag）。
std::string EtagFromCrc(std::uint32_t crc) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08x", crc);
  return std::string(buf);
}

// request_id (string) → uint64，填入 GdsPutReq.requestId_ 供 backend 日志关联。
std::uint64_t RequestIdHash(const std::string& rid) {
  return std::hash<std::string>{}(rid);
}

}  // namespace

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
                               int timeout_ms, std::uint32_t setid)
    : timeout_ms_(timeout_ms), setid_(setid) {
  if (backend_endpoint.empty()) {
    LOG_SYS_WARN("backend_endpoint empty, single-step PUT will reject as "
                 "PROXY_ERR_BACKEND_UNAVAILABLE");
    return;
  }
  std::string host;
  int port = 0;
  if (!ParseEndpoint(backend_endpoint, host, port)) {
    LOG_SYS_WARN("backend_endpoint '{}' parse failed (expect host:port), "
                 "single-step PUT disabled", backend_endpoint);
    return;
  }
  conn_ = std::make_unique<TcpConnection>(host, port, timeout_ms_);
  LOG_SYS_INFO("backend forward connection ready at {} (setid={}, timeout {}ms)",
               backend_endpoint, setid_, timeout_ms_);
}

int BackendGateway::ForwardGdsPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (conn_ == nullptr) {
    LOG_WARN(rid, "backend connection unavailable");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }

  const std::string key = request.bucket() + "/" + request.key();
  if (key.size() > kKeyMaxLen) {
    LOG_WARN(rid, "key too long ({} > {} bytes) for ufile-ac backend bucket={}/{}",
             key.size(), kKeyMaxLen, request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  const auto& token = request.gds_source().rdma_token();
  if (token.empty()) {
    LOG_WARN(rid, "gds rdma_token empty bucket={}/{}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  LOG_DEBUG(rid, "sending GdsPut to backend bucket={}/{} size={} klen={} tlen={}",
            request.bucket(), request.key(), request.object_size(),
            key.size(), token.size());

  // 请求缓冲：Message + GdsPutReq + key + rdma_token
  Message msg{};
  GdsPutReq req{};
  req.keyLen_    = static_cast<std::uint32_t>(key.size());
  req.tokenLen_  = static_cast<std::uint32_t>(token.size());
  req.dataLen_   = request.object_size();
  req.gpuOffset_ = 0;
  req.requestId_ = RequestIdHash(rid);
  req.flags_     = 0;

  const std::uint32_t body_len = sizeof(GdsPutReq) +
      static_cast<std::uint32_t>(key.size() + token.size());
  const std::size_t total = sizeof(Message) + body_len;
  msg.magic_    = MESSAGE_MAGIC_NUMBER;
  msg.version_  = MESSAGE_VERSION_NUMBER;
  msg.type_     = OSD_GDS_PUT_REQ;
  msg.setid_    = setid_;
  msg.bodyLen_  = body_len;
  msg.msgSize_  = htonl(static_cast<std::uint32_t>(total - sizeof(std::uint32_t)));

  std::string buf;
  buf.reserve(total);
  buf.append(reinterpret_cast<const char*>(&msg), sizeof(Message));
  buf.append(reinterpret_cast<const char*>(&req), sizeof(GdsPutReq));
  buf.append(key);
  buf.append(token);

  std::string err;
  Message rsp_msg{};
  GdsPutRsp rsp_body{};
  std::string etag_bytes;
  std::string errmsg;
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_->SendAll(buf.data(), buf.size(), err)) {
      LOG_ERROR(rid, "backend GdsPut send failed: {}", err);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (!conn_->RecvAll(&rsp_msg, sizeof(Message), err)) {
      LOG_ERROR(rid, "backend GdsPut recv header failed: {}", err);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (rsp_msg.magic_ != MESSAGE_MAGIC_NUMBER ||
        rsp_msg.type_ != OSD_GDS_PUT_RSP) {
      const auto rsp_magic = rsp_msg.magic_;
      const auto rsp_type  = rsp_msg.type_;
      LOG_ERROR(rid, "backend GdsPut bad response magic={:x} type={}",
                rsp_magic, rsp_type);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (!conn_->RecvAll(&rsp_body, sizeof(GdsPutRsp), err)) {
      LOG_ERROR(rid, "backend GdsPut recv body failed: {}", err);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (rsp_body.etagLen_ > 0) {
      etag_bytes.resize(rsp_body.etagLen_);
      if (!conn_->RecvAll(&etag_bytes[0], etag_bytes.size(), err)) {
        LOG_ERROR(rid, "backend GdsPut recv etag failed: {}", err);
        return PROXY_ERR_BACKEND_RPC;
      }
    }
    if (rsp_body.errMsgLen_ > 0) {
      errmsg.resize(rsp_body.errMsgLen_);
      if (!conn_->RecvAll(&errmsg[0], errmsg.size(), err)) {
        LOG_ERROR(rid, "backend GdsPut recv errmsg failed: {}", err);
        return PROXY_ERR_BACKEND_RPC;
      }
    }
  }

  if (rsp_body.retcode_ != 0) {
    const auto retcode = rsp_body.retcode_;
    LOG_ERROR(rid, "backend GdsPut failed ret={} errmsg={}", retcode, errmsg);
    return PROXY_ERR_BACKEND_RPC;
  }

  out.crc32c        = rsp_body.crc32c_;
  out.bytes_written = rsp_body.bytesWritten_;
  out.etag          = etag_bytes.empty() ? EtagFromCrc(rsp_body.crc32c_)
                                         : etag_bytes;
  return 0;
}

int BackendGateway::ForwardUcxPut(
    const ::us3_turbo::proxy::ClientProxyPutRequest& request,
    PutOutput& out) {
  const std::string& rid = request.request_id();

  if (conn_ == nullptr) {
    LOG_WARN(rid, "backend connection unavailable");
    return PROXY_ERR_BACKEND_UNAVAILABLE;
  }

  const std::string key = request.bucket() + "/" + request.key();
  if (key.size() > kKeyMaxLen) {
    LOG_WARN(rid, "key too long ({} > {} bytes) for ufile-ac backend bucket={}/{}",
             key.size(), kKeyMaxLen, request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  const auto& src = request.ucx_source();
  const auto& ucx_addr = src.client_ucx_addr();
  const auto& packed_rkey = src.packed_rkey();

  // 对齐 backend OSD_UCX_PUT_REQ 入参校验（ac_server.cc:379-421）：非法入参在 proxy
  // 本地即返回 PROXY_ERR_INVALID_PARAM，避免发到 backend 被拒后笼统返回
  // PROXY_ERR_BACKEND_RPC。校验须在 "sending UcxPut" 日志之前，避免为注定失败的请求
  // 打发送日志。
  if (ucx_addr.empty()) {
    LOG_WARN(rid, "ucx client_ucx_addr empty bucket={}/{}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (packed_rkey.empty() || packed_rkey.size() > kRkeyMaxLen) {
    LOG_WARN(rid, "ucx packed_rkey size {} invalid (expect 1..{}) bucket={}/{}",
             packed_rkey.size(), kRkeyMaxLen, request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }
  if (src.remote_addr() == 0) {
    LOG_WARN(rid, "ucx remote_addr=0 bucket={}/{}",
             request.bucket(), request.key());
    return PROXY_ERR_INVALID_PARAM;
  }

  LOG_DEBUG(rid, "sending UcxPut to backend bucket={}/{} size={} klen={} alen={} rlen={}",
            request.bucket(), request.key(), request.object_size(),
            key.size(), ucx_addr.size(), packed_rkey.size());

  Message msg{};
  UcxPutReq req{};
  req.keyLen_      = static_cast<std::uint32_t>(key.size());
  req.addrLen_     = static_cast<std::uint32_t>(ucx_addr.size());
  req.rkeyLen_     = static_cast<std::uint32_t>(packed_rkey.size());
  req.reserved0_   = 0;
  req.dataLen_     = request.object_size();
  req.remoteAddr_  = src.remote_addr();
  req.sourceOffset_ = 0;
  req.requestId_   = RequestIdHash(rid);
  req.flags_       = 0;

  const std::uint32_t body_len = sizeof(UcxPutReq) +
      static_cast<std::uint32_t>(key.size() + ucx_addr.size() + packed_rkey.size());
  const std::size_t total = sizeof(Message) + body_len;
  msg.magic_    = MESSAGE_MAGIC_NUMBER;
  msg.version_  = MESSAGE_VERSION_NUMBER;
  msg.type_     = OSD_UCX_PUT_REQ;
  msg.setid_    = setid_;
  msg.bodyLen_  = body_len;
  msg.msgSize_  = htonl(static_cast<std::uint32_t>(total - sizeof(std::uint32_t)));

  std::string buf;
  buf.reserve(total);
  buf.append(reinterpret_cast<const char*>(&msg), sizeof(Message));
  buf.append(reinterpret_cast<const char*>(&req), sizeof(UcxPutReq));
  buf.append(key);
  buf.append(ucx_addr);
  buf.append(packed_rkey);

  std::string err;
  Message rsp_msg{};
  UcxPutRsp rsp_body{};
  std::string errmsg;
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_->SendAll(buf.data(), buf.size(), err)) {
      LOG_ERROR(rid, "backend UcxPut send failed: {}", err);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (!conn_->RecvAll(&rsp_msg, sizeof(Message), err)) {
      LOG_ERROR(rid, "backend UcxPut recv header failed: {}", err);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (rsp_msg.magic_ != MESSAGE_MAGIC_NUMBER ||
        rsp_msg.type_ != OSD_UCX_PUT_RSP) {
      const auto rsp_magic = rsp_msg.magic_;
      const auto rsp_type  = rsp_msg.type_;
      LOG_ERROR(rid, "backend UcxPut bad response magic={:x} type={}",
                rsp_magic, rsp_type);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (!conn_->RecvAll(&rsp_body, sizeof(UcxPutRsp), err)) {
      LOG_ERROR(rid, "backend UcxPut recv body failed: {}", err);
      return PROXY_ERR_BACKEND_RPC;
    }
    if (rsp_body.errMsgLen_ > 0) {
      errmsg.resize(rsp_body.errMsgLen_);
      if (!conn_->RecvAll(&errmsg[0], errmsg.size(), err)) {
        LOG_ERROR(rid, "backend UcxPut recv errmsg failed: {}", err);
        return PROXY_ERR_BACKEND_RPC;
      }
    }
  }

  if (rsp_body.retcode_ != 0) {
    const auto retcode = rsp_body.retcode_;
    LOG_ERROR(rid, "backend UcxPut failed ret={} errmsg={}", retcode, errmsg);
    return PROXY_ERR_BACKEND_RPC;
  }

  out.crc32c        = rsp_body.crc32c_;
  out.bytes_written = rsp_body.bytesWritten_;
  out.etag          = EtagFromCrc(rsp_body.crc32c_);  // UcxPutRsp 无 etag 字段
  return 0;
}

}  // namespace us3_turbo::proxy
