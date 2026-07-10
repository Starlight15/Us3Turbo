#include "proxy/src/storage/ufile_ac_protocol.h"

#include <arpa/inet.h>  // htonl

#include <cstring>   // std::memcpy
#include <string>

namespace us3_turbo::proxy {

// ============================ GDS PUT ============================

/* 编码 GDS PUT 请求：Message(52) + GdsPutReq(52) + key + rdma_token。
 * 仅 msgSize_ 大端，其余主机序直接赋值；预留字段填 0。返回总字节数。 */
std::size_t EncodeGdsPutRequest(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(rdma_token.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(GDS_PUT_REQ_SIZE + key_len + tok_len);
  const std::uint32_t msg_size_field =
      static_cast<std::uint32_t>(MESSAGE_HEAD_SIZE + body_len -
                                 sizeof(std::uint32_t));
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = out_buffer.data();

  // 请求头（仅 msgSize_ 大端，其余主机序直接赋值）
  Message msg{};
  msg.msgSize_       = htonl(msg_size_field);
  msg.magic_         = MESSAGE_MAGIC_NUMBER;
  msg.version_       = MESSAGE_VERSION_NUMBER;
  msg.type_          = OSD_GDS_PUT_REQ;
  msg.flowno_        = 0;
  msg.sessionIdLow_  = session_id;
  msg.sessionIdHigh_ = 0;
  msg.setid_         = setid;
  msg.payload_       = 0;
  msg.bodyLen_       = body_len;
  std::memcpy(p, &msg, MESSAGE_HEAD_SIZE);
  p += MESSAGE_HEAD_SIZE;

  // GdsPutReq（requestId_/sessionId*_/flags_ 预留填 0）
  GdsPutReq req{};
  req.keyLen_        = key_len;
  req.tokenLen_      = tok_len;
  req.dataLen_       = data_len;
  req.gpuOffset_     = gpu_offset;
  req.requestId_     = 0;
  req.sessionIdLow_  = 0;
  req.sessionIdHigh_ = 0;
  req.flags_         = 0;
  std::memcpy(p, &req, GDS_PUT_REQ_SIZE);
  p += GDS_PUT_REQ_SIZE;

  // 变长数据：key + rdma_token
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, rdma_token.data(), rdma_token.size());
  return total;
}

/* 解码 GDS PUT 响应体（GdsPutRsp + errmsg）。返回 0=成功，-1=格式错误。 */
int DecodeGdsPutResponse(
    const char* buffer,
    std::size_t len,
    GdsPutRsp& out_rsp,
    std::string& out_err) {
  if (len < GDS_PUT_RSP_SIZE) {
    out_err = "GdsPut rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, GDS_PUT_RSP_SIZE);
  const std::uint32_t etag_len   = out_rsp.etagLen_;
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - GDS_PUT_RSP_SIZE;
  if (static_cast<std::size_t>(etag_len) + errmsg_len > var_len) {
    out_err = "GdsPut rsp var overflow etag=" + std::to_string(etag_len) +
              " errmsg=" + std::to_string(errmsg_len) +
              " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + GDS_PUT_RSP_SIZE + etag_len, errmsg_len);
  return 0;
}

// ============================ UCX PUT ============================

/* 编码 UCX PUT 请求：Message(52) + UcxPutReq(68) + key + client_ucx_addr + packed_rkey。
 * 仅 msgSize_ 大端，其余主机序；预留字段填 0。返回总字节数。 */
std::size_t EncodeUcxPutRequest(
    const std::string& key,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    std::uint64_t source_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer) {
  const std::uint32_t key_len  = static_cast<std::uint32_t>(key.size());
  const std::uint32_t addr_len = static_cast<std::uint32_t>(client_ucx_addr.size());
  const std::uint32_t rkey_len = static_cast<std::uint32_t>(packed_rkey.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(UCX_PUT_REQ_SIZE + key_len + addr_len + rkey_len);
  const std::uint32_t msg_size_field =
      static_cast<std::uint32_t>(MESSAGE_HEAD_SIZE + body_len -
                                 sizeof(std::uint32_t));
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = out_buffer.data();

  // 请求头（仅 msgSize_ 大端，其余主机序）
  Message msg{};
  msg.msgSize_       = htonl(msg_size_field);
  msg.magic_         = MESSAGE_MAGIC_NUMBER;
  msg.version_       = MESSAGE_VERSION_NUMBER;
  msg.type_          = OSD_UCX_PUT_REQ;
  msg.flowno_        = 0;
  msg.sessionIdLow_  = session_id;
  msg.sessionIdHigh_ = 0;
  msg.setid_         = setid;
  msg.payload_       = 0;
  msg.bodyLen_       = body_len;
  std::memcpy(p, &msg, MESSAGE_HEAD_SIZE);
  p += MESSAGE_HEAD_SIZE;

  // UcxPutReq（reserved0_/requestId_/sessionId*_/flags_ 预留填 0）
  UcxPutReq req{};
  req.keyLen_        = key_len;
  req.addrLen_       = addr_len;
  req.rkeyLen_       = rkey_len;
  req.reserved0_     = 0;
  req.dataLen_       = data_len;
  req.remoteAddr_    = remote_addr;
  req.sourceOffset_  = source_offset;
  req.requestId_     = 0;
  req.sessionIdLow_  = 0;
  req.sessionIdHigh_ = 0;
  req.flags_         = 0;
  std::memcpy(p, &req, UCX_PUT_REQ_SIZE);
  p += UCX_PUT_REQ_SIZE;

  // 变长数据：key + client_ucx_addr + packed_rkey
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, client_ucx_addr.data(), client_ucx_addr.size());
  p += client_ucx_addr.size();
  std::memcpy(p, packed_rkey.data(), packed_rkey.size());
  return total;
}

/* 解码 UCX PUT 响应体（UcxPutRsp + errmsg）。返回 0=成功，-1=格式错误。 */
int DecodeUcxPutResponse(
    const char* buffer,
    std::size_t len,
    UcxPutRsp& out_rsp,
    std::string& out_err) {
  if (len < UCX_PUT_RSP_SIZE) {
    out_err = "UcxPut rsp body too short len=" + std::to_string(len);
    return -1;
  }
  std::memcpy(&out_rsp, buffer, UCX_PUT_RSP_SIZE);
  const std::uint32_t errmsg_len = out_rsp.errMsgLen_;
  const std::size_t var_len = len - UCX_PUT_RSP_SIZE;
  if (static_cast<std::size_t>(errmsg_len) > var_len) {
    out_err = "UcxPut rsp var overflow errmsg=" + std::to_string(errmsg_len) +
              " var=" + std::to_string(var_len);
    return -1;
  }
  out_err.assign(buffer + UCX_PUT_RSP_SIZE, errmsg_len);
  return 0;
}

// ============================ DEL ============================

/* 编码 DEL 请求：Message(52) + DelReq(12) + key。
 * 仅 msgSize_ 大端，其余主机序；reserve_ 填 0。返回总字节数。 */
std::size_t EncodeDelRequest(
    const std::string& key,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(DEL_REQ_SIZE + key_len);
  const std::uint32_t msg_size_field =
      static_cast<std::uint32_t>(MESSAGE_HEAD_SIZE + body_len -
                                 sizeof(std::uint32_t));
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = out_buffer.data();

  // 请求头（仅 msgSize_ 大端，其余主机序）
  Message msg{};
  msg.msgSize_       = htonl(msg_size_field);
  msg.magic_         = MESSAGE_MAGIC_NUMBER;
  msg.version_       = MESSAGE_VERSION_NUMBER;
  msg.type_          = OSD_DEL_REQ;
  msg.flowno_        = 0;
  msg.sessionIdLow_  = session_id;
  msg.sessionIdHigh_ = 0;
  msg.setid_         = setid;
  msg.payload_       = 0;
  msg.bodyLen_       = body_len;
  std::memcpy(p, &msg, MESSAGE_HEAD_SIZE);
  p += MESSAGE_HEAD_SIZE;

  // DelReq（reserve_ 保留填 0）
  DelReq req{};
  req.keyLen_  = key_len;
  req.reserve_ = 0;
  std::memcpy(p, &req, DEL_REQ_SIZE);
  p += DEL_REQ_SIZE;

  // 变长数据：key
  std::memcpy(p, key.data(), key.size());
  return total;
}

/* 解码 DEL 响应体（DelRsp + errmsg）。返回 0=成功，-1=格式错误。 */
int DecodeDelResponse(
    const char* buffer,
    std::size_t len,
    DelRsp& out_rsp,
    std::string& out_err) {
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

/* 编码 GDS GET 请求：Message(52) + GdsGetReq(60) + key + rdma_token。
 * 与 GdsPutReq 布局不同：tokenLen_ 之后多 readOffset_（8字节）。
 * 仅 msgSize_ 大端，其余主机序直接赋值；预留字段填 0。返回总字节数。 */
std::size_t EncodeGdsGetRequest(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t read_offset,
    std::uint64_t gpu_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer) {
  const std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
  const std::uint32_t tok_len = static_cast<std::uint32_t>(rdma_token.size());
  const std::uint32_t body_len =
      static_cast<std::uint32_t>(GDS_GET_REQ_SIZE + key_len + tok_len);
  const std::uint32_t msg_size_field =
      static_cast<std::uint32_t>(MESSAGE_HEAD_SIZE + body_len -
                                 sizeof(std::uint32_t));
  const std::size_t total = MESSAGE_HEAD_SIZE + body_len;

  out_buffer.resize(total);
  char* p = out_buffer.data();

  // 请求头（仅 msgSize_ 大端，其余主机序直接赋值）
  Message msg{};
  msg.msgSize_       = htonl(msg_size_field);
  msg.magic_         = MESSAGE_MAGIC_NUMBER;
  msg.version_       = MESSAGE_VERSION_NUMBER;
  msg.type_          = OSD_GDS_GET_REQ;
  msg.flowno_        = 0;
  msg.sessionIdLow_  = session_id;
  msg.sessionIdHigh_ = 0;
  msg.setid_         = setid;
  msg.payload_       = 0;
  msg.bodyLen_       = body_len;
  std::memcpy(p, &msg, MESSAGE_HEAD_SIZE);
  p += MESSAGE_HEAD_SIZE;

  // GdsGetReq（readOffset_ 本阶段恒 0；requestId_/sessionId*_/flags_ 预留填 0）
  GdsGetReq req{};
  req.keyLen_        = key_len;
  req.tokenLen_      = tok_len;
  req.readOffset_    = read_offset;
  req.dataLen_       = data_len;
  req.gpuOffset_     = gpu_offset;
  req.requestId_     = 0;
  req.sessionIdLow_  = 0;
  req.sessionIdHigh_ = 0;
  req.flags_         = 0;
  std::memcpy(p, &req, GDS_GET_REQ_SIZE);
  p += GDS_GET_REQ_SIZE;

  // 变长数据：key + rdma_token
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  std::memcpy(p, rdma_token.data(), rdma_token.size());
  return total;
}

/* 解码 GDS GET 响应体（GdsGetRsp + errmsg，无 etag）。
 * 对齐 DecodeUcxPutResponse 的写法（同为无 etag 的响应）。返回 0=成功，-1=格式错误。 */
int DecodeGdsGetResponse(
    const char* buffer,
    std::size_t len,
    GdsGetRsp& out_rsp,
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

}  // namespace us3_turbo::proxy
