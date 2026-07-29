#include "proxy/src/storage/ufile_ac_protocol.h"

#include <arpa/inet.h>  // htonl
#include <cstring>      // std::memcpy
#include <string>

namespace us3_turbo::proxy {

namespace {

/* 将 Message 头写入 buf，返回指针跳过头部 */
inline char* WriteHeader(char* buf, std::uint32_t type, std::uint32_t body_len,
                         std::uint32_t setid, std::uint64_t session_id) {
  Message msg{};
  msg.msgSize_ = htonl(MESSAGE_HEAD_SIZE + body_len - sizeof(std::uint32_t));
  msg.magic_ = MESSAGE_MAGIC_NUMBER;
  msg.version_ = MESSAGE_VERSION_NUMBER;
  msg.type_ = type;
  msg.flowno_ = 0;
  msg.sessionIdLow_ = session_id;
  msg.sessionIdHigh_ = 0;
  msg.setid_ = setid;
  msg.payload_ = 0;
  msg.bodyLen_ = body_len;
  std::memcpy(buf, &msg, MESSAGE_HEAD_SIZE);
  return buf + MESSAGE_HEAD_SIZE;
}

}  // namespace

// ============================ GDS PUT ============================

std::size_t EncodeGdsPutRequest(const std::string& key, const std::string& rdma_token,
                                std::uint64_t gpu_offset, std::uint64_t data_len,
                                std::uint32_t setid, std::uint64_t session_id,
                                std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(rdma_token.size());
  const std::uint32_t body_len = static_cast<std::uint32_t>(GDS_PUT_REQ_SIZE + key_len + tok_len);
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = WriteHeader(out_buffer.data(), OSD_GDS_PUT_REQ, body_len, setid, session_id);

  /* GdsPutReq: 预留字段填 0 */
  GdsPutReq req{};
  req.keyLen_ = key_len;
  req.tokenLen_ = tok_len;
  req.dataLen_ = data_len;
  req.gpuOffset_ = gpu_offset;
  req.requestId_ = 0;
  req.sessionIdLow_ = 0;
  req.sessionIdHigh_ = 0;
  req.flags_ = 0;
  std::memcpy(p, &req, GDS_PUT_REQ_SIZE);
  p += GDS_PUT_REQ_SIZE;

  /* 变长数据: key + rdma_token */
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, rdma_token.data(), rdma_token.size());
  return total;
}

int DecodeGdsPutResponse(const char* buffer, std::size_t len, GdsPutRsp& out_rsp,
                         std::string& out_err) {
  if (len < GDS_PUT_RSP_SIZE) {
    out_err = "GdsPut rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, GDS_PUT_RSP_SIZE);
  const std::uint32_t etag_len = out_rsp.etagLen_;
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - GDS_PUT_RSP_SIZE;
  if (static_cast<std::size_t>(etag_len) + errmsg_len > var_len) {
    out_err = "GdsPut rsp var overflow etag=" + std::to_string(etag_len) +
              " errmsg=" + std::to_string(errmsg_len) + " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + GDS_PUT_RSP_SIZE + etag_len, errmsg_len);
  return 0;
}

// ============================ DEL ============================

std::size_t EncodeDelRequest(const std::string& key, std::uint32_t setid, std::uint64_t session_id,
                             std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t body_len = static_cast<std::uint32_t>(DEL_REQ_SIZE + key_len);
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = WriteHeader(out_buffer.data(), OSD_DEL_REQ, body_len, setid, session_id);

  /* DelReq: reserve_ 填 0 */
  DelReq req{};
  req.keyLen_ = key_len;
  req.reserve_ = 0;
  std::memcpy(p, &req, DEL_REQ_SIZE);
  p += DEL_REQ_SIZE;

  /* 变长数据: key */
  std::memcpy(p, key.data(), key.size());
  return total;
}

int DecodeDelResponse(const char* buffer, std::size_t len, DelRsp& out_rsp, std::string& out_err) {
  if (len < DEL_RSP_SIZE) {
    out_err = "Del rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, DEL_RSP_SIZE);
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - DEL_RSP_SIZE;
  if (static_cast<std::size_t>(errmsg_len) > var_len) {
    out_err = "Del rsp var overflow errmsg=" + std::to_string(errmsg_len) +
              " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + DEL_RSP_SIZE, errmsg_len);
  return 0;
}

// ============================ GDS GET ============================

std::size_t EncodeGdsGetRequest(const std::string& key, const std::string& rdma_token,
                                std::uint64_t read_offset, std::uint64_t gpu_offset,
                                std::uint64_t data_len, std::uint32_t setid,
                                std::uint64_t session_id, std::uint64_t request_id,
                                std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(rdma_token.size());
  const std::uint32_t body_len = static_cast<std::uint32_t>(GDS_GET_REQ_SIZE + key_len + tok_len);
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = WriteHeader(out_buffer.data(), OSD_GDS_GET_REQ, body_len, setid, session_id);

  /* GdsGetReq: readOffset_ 恒 0, 预留字段填 0 */
  GdsGetReq req{};
  req.keyLen_ = key_len;
  req.tokenLen_ = tok_len;
  req.readOffset_ = read_offset;
  req.dataLen_ = data_len;
  req.gpuOffset_ = gpu_offset;
  req.requestId_ = request_id;
  req.sessionIdLow_ = 0;
  req.sessionIdHigh_ = 0;
  req.flags_ = 0;
  std::memcpy(p, &req, GDS_GET_REQ_SIZE);
  p += GDS_GET_REQ_SIZE;

  /* 变长数据: key + rdma_token */
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, rdma_token.data(), rdma_token.size());
  return total;
}

int DecodeGdsGetResponse(const char* buffer, std::size_t len, GdsGetRsp& out_rsp,
                         std::string& out_err) {
  if (len < GDS_GET_RSP_SIZE) {
    out_err = "GdsGet rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, GDS_GET_RSP_SIZE);
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - GDS_GET_RSP_SIZE;
  if (static_cast<std::size_t>(errmsg_len) > var_len) {
    out_err = "GdsGet rsp var overflow errmsg=" + std::to_string(errmsg_len) +
              " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + GDS_GET_RSP_SIZE, errmsg_len);
  return 0;
}

// ============================ RDMA PUT ============================

std::size_t EncodeRdmaPutRequest(const std::string& key, const std::string& token,
                                 std::uint64_t source_offset, std::uint64_t data_len,
                                 std::uint32_t setid, std::uint64_t session_id,
                                 std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(token.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(RDMA_PUT_REQ_SIZE + key_len + tok_len);
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = WriteHeader(out_buffer.data(), OSD_RDMA_PUT_REQ, body_len, setid, session_id);

  /* RdmaPutReq: 预留字段填 0 */
  RdmaPutReq req{};
  req.keyLen_ = key_len;
  req.tokenLen_ = tok_len;
  req.dataLen_ = data_len;
  req.sourceOffset_ = source_offset;
  req.requestId_ = 0;
  req.sessionIdLow_ = 0;
  req.sessionIdHigh_ = 0;
  req.flags_ = 0;
  std::memcpy(p, &req, RDMA_PUT_REQ_SIZE);
  p += RDMA_PUT_REQ_SIZE;

  /* 变长数据: key + token */
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, token.data(), token.size());
  return total;
}

int DecodeRdmaPutResponse(const char* buffer, std::size_t len, RdmaPutRsp& out_rsp,
                           std::string& out_err) {
  if (len < RDMA_PUT_RSP_SIZE) {
    out_err = "RdmaPut rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, RDMA_PUT_RSP_SIZE);
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - RDMA_PUT_RSP_SIZE;
  if (static_cast<std::size_t>(errmsg_len) > var_len) {
    out_err = "RdmaPut rsp var overflow errmsg=" + std::to_string(errmsg_len) +
              " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + RDMA_PUT_RSP_SIZE, errmsg_len);
  return 0;
}

// ============================ RDMA GET ============================

std::size_t EncodeRdmaGetRequest(const std::string& key, const std::string& token,
                                 std::uint64_t read_offset, std::uint64_t dest_offset,
                                 std::uint64_t data_len, std::uint32_t setid,
                                 std::uint64_t session_id, std::uint64_t request_id,
                                 std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(token.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(RDMA_GET_REQ_SIZE + key_len + tok_len);
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = WriteHeader(out_buffer.data(), OSD_RDMA_GET_REQ, body_len, setid, session_id);

  /* RdmaGetReq: 预留字段填 0 */
  RdmaGetReq req{};
  req.keyLen_ = key_len;
  req.tokenLen_ = tok_len;
  req.readOffset_ = read_offset;
  req.dataLen_ = data_len;
  req.destOffset_ = dest_offset;
  req.requestId_ = request_id;
  req.sessionIdLow_ = 0;
  req.sessionIdHigh_ = 0;
  req.flags_ = 0;
  std::memcpy(p, &req, RDMA_GET_REQ_SIZE);
  p += RDMA_GET_REQ_SIZE;

  /* 变长数据: key + token */
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, token.data(), token.size());
  return total;
}

int DecodeRdmaGetResponse(const char* buffer, std::size_t len, RdmaGetRsp& out_rsp,
                           std::string& out_err) {
  if (len < RDMA_GET_RSP_SIZE) {
    out_err = "RdmaGet rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, RDMA_GET_RSP_SIZE);
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - RDMA_GET_RSP_SIZE;
  if (static_cast<std::size_t>(errmsg_len) > var_len) {
    out_err = "RdmaGet rsp var overflow errmsg=" + std::to_string(errmsg_len) +
              " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + RDMA_GET_RSP_SIZE, errmsg_len);
  return 0;
}

}  // namespace us3_turbo::proxy
