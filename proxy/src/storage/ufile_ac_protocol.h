#pragma once

/*
 * ufile_ac_protocol — proxy → backend (ufile-ac) 二进制协议定义 + 编解码。
 *
 * 同源 ufile-ac/message.h（拷贝，非 include —— ufile-ac 不在本仓构建树内）。
 * 修改须与上游一致；差异以源码为准，禁止手抄字段。
 *
 * 字节序（核实自 ac_server.cc / gds_service.cc）：
 *   - Message.msgSize_ 为网络序（htonl 收发）；
 *   - 其余所有字段主机序，直接赋值，禁止 htonl/ntohl。
 *   - setid_ 必填且须 == backend g_setid，否则 ForceClose。
 *   - GdsPutRsp.etagLen_ 服务端恒 0，proxy 解析时 etag 置空（容错）。
 *
 * 各 struct packed 无填充；柔性数组不计入 sizeof。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace us3_turbo::proxy {

// ---- 协议常量（对齐 ufile-ac/message.h）----
constexpr std::uint32_t MESSAGE_MAGIC_NUMBER   = 0x0a0a0a0a;
constexpr std::uint32_t MESSAGE_VERSION_NUMBER = 0x01;
constexpr std::uint32_t KEY_MAX_LENGTH          = 48;
constexpr std::uint64_t MAX_VALUE_LENGTH        = 16777216ULL;  // 16MB

// 消息类型（对齐 ufile-ac message.h MessageType，仅列本仓用到的）
enum MessageType : std::uint32_t {
  OSD_DEL_REQ     = 7,
  OSD_DEL_RSP     = 8,
  OSD_GDS_PUT_REQ = 21,
  OSD_GDS_PUT_RSP = 22,
  OSD_UCX_PUT_REQ = 25,
  OSD_UCX_PUT_RSP = 26,
};

/*
 * 通用消息头（sizeof = 52，packed）。body_ 柔性数组不计入 sizeof。
 *   data_   = body_（变长，按 type 解释为各 Req/Rsp）
 *   bodyLen_ = body_ 长度（主机序）
 */
struct Message {
  std::uint32_t msgSize_;          // 消息总长度（不含自身 4 字节），BigEndian
  std::uint32_t magic_;            // MESSAGE_MAGIC_NUMBER
  std::uint32_t version_;          // MESSAGE_VERSION_NUMBER
  std::uint32_t type_;             // MessageType
  std::uint32_t flowno_;           // flow number，原样返回（预留填 0）
  std::uint64_t sessionIdLow_;     // 服务端统计用，填自增值
  std::uint64_t sessionIdHigh_;    // 预留
  std::uint32_t setid_;            // 必填，== backend g_setid
  std::uint64_t payload_;          // 业务信息，预留填 0
  std::uint32_t bodyLen_;          // body_ 长度
  char          body_[0];          // 柔性数组
} __attribute__((packed));

/*
 * GDS PUT 请求（sizeof = 52）+ 变长 data_。
 *   data_   = key bytes || rdma_token bytes
 *   bodyLen_ = GDS_PUT_REQ_SIZE + keyLen_ + tokenLen_
 */
struct GdsPutReq {
  std::uint32_t keyLen_;           // key 长度
  std::uint32_t tokenLen_;         // rdma_token 长度
  std::uint64_t dataLen_;          // 数据长度
  std::uint64_t gpuOffset_;        // chunk 在 GPU buffer 偏移
  std::uint64_t requestId_;        // 预留填 0
  std::uint64_t sessionIdLow_;     // 预留填 0
  std::uint64_t sessionIdHigh_;    // 预留填 0
  std::uint32_t flags_;            // 预留填 0
  char          data_[0];          // key + rdma_token
} __attribute__((packed));

/*
 * GDS PUT 响应（sizeof = 24）+ 变长 data_。
 *   data_   = etag bytes(etagLen_) || errmsg bytes(errMsgLen_)
 *   注：backend etagLen_ 恒 0（不返回 etag）。
 */
struct GdsPutRsp {
  std::int32_t  retcode_;          // 0=成功
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t etagLen_;          // backend 恒 0
  std::uint32_t errMsgLen_;
  char          data_[0];          // etag + errmsg
} __attribute__((packed));

/*
 * UCX PUT 请求（sizeof = 68）+ 变长 data_。
 *   data_   = key bytes || client_ucx_addr bytes || packed_rkey bytes
 *   bodyLen_ = UCX_PUT_REQ_SIZE + keyLen_ + addrLen_ + rkeyLen_
 */
struct UcxPutReq {
  std::uint32_t keyLen_;           // key 长度
  std::uint32_t addrLen_;          // client_ucx_addr 长度
  std::uint32_t rkeyLen_;          // packed_rkey 长度
  std::uint32_t reserved0_;       // 预留填 0
  std::uint64_t dataLen_;          // 数据长度
  std::uint64_t remoteAddr_;       // client source buffer 虚拟地址
  std::uint64_t sourceOffset_;     // chunk 偏移
  std::uint64_t requestId_;       // 预留填 0
  std::uint64_t sessionIdLow_;     // 预留填 0
  std::uint64_t sessionIdHigh_;    // 预留填 0
  std::uint32_t flags_;            // 预留填 0
  char          data_[0];          // key + client_ucx_addr + packed_rkey
} __attribute__((packed));

/*
 * UCX PUT 响应（sizeof = 20）+ 变长 data_（errmsg bytes，无 etag）。
 *   bodyLen_ = UCX_PUT_RSP_SIZE + errMsgLen_
 */
struct UcxPutRsp {
  std::int32_t  retcode_;          // 0=成功
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t errMsgLen_;
  char          data_[0];          // errmsg bytes
} __attribute__((packed));

/*
 * DEL 请求（sizeof = 12）+ 变长 key_。删除一个对象（block）。
 *   bodyLen_ = DEL_REQ_SIZE + keyLen_
 *   服务端支持 key_ 内多 key 逗号分隔走 DelBatch；本仓单 key 走 Del。
 */
struct DelReq {
  std::uint32_t keyLen_;           // key 长度
  std::uint64_t reserve_;          // 保留填 0
  char          key_[0];            // key bytes
} __attribute__((packed));

/*
 * DEL 响应（sizeof = 8）+ 变长 data_（errmsg bytes）。
 *   retcode_ KEY_NOT_FOUND 在清理场景可接受，调用方按 WARN 处理。
 */
struct DelRsp {
  std::int32_t  retcode_;          // 0=成功
  std::uint32_t errMsgLen_;
  char          data_[0];          // errmsg bytes
} __attribute__((packed));

// ---- 尺寸常量（用 sizeof，避免硬编码笔误）----
constexpr std::size_t MESSAGE_HEAD_SIZE = sizeof(Message);
constexpr std::size_t GDS_PUT_REQ_SIZE  = sizeof(GdsPutReq);
constexpr std::size_t GDS_PUT_RSP_SIZE  = sizeof(GdsPutRsp);
constexpr std::size_t UCX_PUT_REQ_SIZE  = sizeof(UcxPutReq);
constexpr std::size_t UCX_PUT_RSP_SIZE  = sizeof(UcxPutRsp);
constexpr std::size_t DEL_REQ_SIZE      = sizeof(DelReq);
constexpr std::size_t DEL_RSP_SIZE      = sizeof(DelRsp);

// ============================ 编解码函数 ============================

// 编码 GDS PUT 请求到 out_buffer，返回总字节数。
// buffer = Message(52) + GdsPutReq(52) + key + rdma_token
std::size_t EncodeGdsPutRequest(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer);

// 解码 GDS PUT 响应体（GdsPutRsp + errmsg，不含 Message 头）。
// 返回 0=成功（out_err 填 backend errmsg，可能空）；-1=格式错误（out_err 填描述）。
int DecodeGdsPutResponse(
    const char* buffer,
    std::size_t len,
    GdsPutRsp& out_rsp,
    std::string& out_err);

// 编码 UCX PUT 请求。
// buffer = Message(52) + UcxPutReq(68) + key + client_ucx_addr + packed_rkey
std::size_t EncodeUcxPutRequest(
    const std::string& key,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    std::uint64_t source_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer);

// 解码 UCX PUT 响应体（UcxPutRsp + errmsg）。返回 0=成功，-1=格式错误。
int DecodeUcxPutResponse(
    const char* buffer,
    std::size_t len,
    UcxPutRsp& out_rsp,
    std::string& out_err);

// 编码 DEL 请求到 out_buffer，返回总字节数。
// buffer = Message(52) + DelReq(12) + key
std::size_t EncodeDelRequest(
    const std::string& key,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer);

// 解码 DEL 响应体（DelRsp + errmsg，不含 Message 头）。返回 0=成功，-1=格式错误。
int DecodeDelResponse(
    const char* buffer,
    std::size_t len,
    DelRsp& out_rsp,
    std::string& out_err);

}  // namespace us3_turbo::proxy
