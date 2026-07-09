#pragma once

// ufile_ac_protocol.h — proxy → backend (ufile-ac) 自定义二进制协议定义 + 编解码。
//
// 同源：/mnt/us3_test/ld/ufile-ac/message.h（拷贝，非 include —— ufile-ac 不在本仓
// 构建树内）。修改须与上游 message.h 保持一致；禁止手抄字段，差异以源码为准。
//
// 子阶段2：协议结构体（定义）与编解码（函数）合并到本模块（原 protocol_codec
// 独立模块已删）。Encode/Decode 收原始参数（key/token/addr/...），不依赖 protobuf；
// UfileAcClient 抽参后喂给本模块。命名空间 us3_turbo::proxy（无 codec:: 子层）。
//
// 字节序（已从 ufile-ac 服务端 ac_server.cc / gds_service.cc 核实，doc F2）：
// - Message.msgSize_   网络字节序（BigEndian）：发送 htonl，服务端写响应也 htonl
//   （gds_service.cc:283）；服务端读请求时不解析此字段值（ac_server.cc:240-243）。
// - Message 其余字段（magic_/version_/type_/flowno_/sessionIdLow_/High_/setid_/
//   payload_/bodyLen_）+ GdsPutReq/Rsp + UcxPutReq/Rsp + DelReq/Rsp 全部字段：主机序
//   （x86=LE），直接赋值，禁止 htonl/ntohl（ac_server.cc:240 `msg.bodyLen_` 无 ntohl）。
// - setid_ 必填且须 == backend g_setid（默认 1），否则 ForceClose（ac_server.cc:260）。
// - GdsPutRsp.etagLen_ 服务端恒为 0（memset 后从不填，gds_service.cc:285-290）：
//   backend 当前不返回 etag，proxy 解析时 etag 置空（容错，doc F7）。
//
// 各 struct __attribute__((packed))，无填充；data_/body_/key_ 为柔性数组（不计入 sizeof）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace us3_turbo::proxy {

// ---- 协议常量（对齐 ufile-ac/message.h）----
constexpr std::uint32_t MESSAGE_MAGIC_NUMBER    = 0x0a0a0a0a;
constexpr std::uint32_t MESSAGE_VERSION_NUMBER  = 0x01;
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

// 通用消息头（sizeof = 52，packed）。body_ 柔性数组不计入 sizeof。
struct Message {
  std::uint32_t msgSize_;          // 消息总长度（不含自己的 4 字节），BigEndian
  std::uint32_t magic_;            // MESSAGE_MAGIC_NUMBER（主机序）
  std::uint32_t version_;          // MESSAGE_VERSION_NUMBER（主机序）
  std::uint32_t type_;             // MessageType（主机序）
  std::uint32_t flowno_;           // flow number，原样返回（预留填 0）
  std::uint64_t sessionIdLow_;     // 仅服务端统计/malloc_trim 用，填自增值即可
  std::uint64_t sessionIdHigh_;    // 预留
  std::uint32_t setid_;            // 必填，== backend g_setid，否则 ForceClose
  std::uint64_t payload_;          // 业务信息（ec replica_nth），预留填 0
  std::uint32_t bodyLen_;          // body_ 长度（主机序，无 ntohl）
  char          body_[0];          // 柔性数组
} __attribute__((packed));

// GDS PUT 请求（sizeof = 52，packed）+ 变长 data_。
//   data_   = key bytes || rdma_token bytes
//   bodyLen_ = GDS_PUT_REQ_SIZE + keyLen_ + tokenLen_（ac_server.cc:338-344）
struct GdsPutReq {
  std::uint32_t keyLen_;           // key 长度，≤ KEY_MAX_LENGTH(48)
  std::uint32_t tokenLen_;         // rdma_token 长度，!= 0
  std::uint64_t dataLen_;          // 数据长度，0 < x ≤ MAX_VALUE_LENGTH
  std::uint64_t gpuOffset_;        // chunk 在 GPU buffer 偏移，单对象填 0
  std::uint64_t requestId_;        // 预留（backend 不读），填 0
  std::uint64_t sessionIdLow_;     // 预留，填 0
  std::uint64_t sessionIdHigh_;    // 预留，填 0
  std::uint32_t flags_;            // 预留，填 0
  char          data_[0];          // key bytes + rdma_token bytes
} __attribute__((packed));

// GDS PUT 响应（sizeof = 24，packed）+ 变长 data_。
//   data_   = etag bytes(etagLen_) || errmsg bytes(errMsgLen_)
//   bodyLen_ = GDS_PUT_RSP_SIZE + errMsgLen_（etag 恒不返回，etagLen_=0）
//   注：当前 backend etagLen_ 恒 0（gds_service.cc:285-290）。
struct GdsPutRsp {
  std::int32_t  retcode_;          // 0=成功
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t etagLen_;          // backend 恒 0
  std::uint32_t errMsgLen_;
  char          data_[0];          // etag bytes + errmsg bytes
} __attribute__((packed));

// UCX PUT 请求（sizeof = 68，packed）+ 变长 data_。
//   data_   = key bytes || client_ucx_addr bytes || packed_rkey bytes
//   bodyLen_ = UCX_PUT_REQ_SIZE + keyLen_ + addrLen_ + rkeyLen_（ac_server.cc:384-391）
struct UcxPutReq {
  std::uint32_t keyLen_;           // 0 < x ≤ KEY_MAX_LENGTH(48)
  std::uint32_t addrLen_;          // client_ucx_addr 长度，!= 0
  std::uint32_t rkeyLen_;          // packed_rkey 长度，0 < x ≤ 65536
  std::uint32_t reserved0_;       // 预留，填 0
  std::uint64_t dataLen_;          // 0 < x ≤ MAX_VALUE_LENGTH
  std::uint64_t remoteAddr_;       // client source buffer 虚拟地址，!= 0
  std::uint64_t sourceOffset_;     // chunk 偏移，单对象填 0
  std::uint64_t requestId_;       // 预留，填 0
  std::uint64_t sessionIdLow_;     // 预留，填 0
  std::uint64_t sessionIdHigh_;    // 预留，填 0
  std::uint32_t flags_;            // 预留，填 0
  char          data_[0];          // key + client_ucx_addr + packed_rkey
} __attribute__((packed));

// UCX PUT 响应（sizeof = 20，packed）+ 变长 data_（errmsg bytes，无 etag）。
//   bodyLen_ = UCX_PUT_RSP_SIZE + errMsgLen_
struct UcxPutRsp {
  std::int32_t  retcode_;          // 0=成功
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t errMsgLen_;
  char          data_[0];          // errmsg bytes
} __attribute__((packed));

// DEL 请求（sizeof = 12，packed）+ 变长 key_。删除一个对象（block）。
//   bodyLen_ = DEL_REQ_SIZE + keyLen_
//   注：ufile-ac 服务端支持 key_ 内多 key 逗号分隔走 DelBatch；本仓单 key 用 Del。
//   (ac_server.cc:477-489, message.h DelReq)
struct DelReq {
  std::uint32_t keyLen_;           // key 长度，≤ KEY_MAX_LENGTH(48)
  std::uint64_t reserve_;          // 保留，填 0
  char          key_[0];            // key bytes
} __attribute__((packed));

// DEL 响应（sizeof = 8，packed）+ 变长 data_（errmsg bytes）。
struct DelRsp {
  std::int32_t  retcode_;          // 0=成功（KEY_NOT_FOUND 亦可能，清理时忽略）
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
// 返回 0=成功（out_err 填 backend 的 errmsg，可能空）；
//        -1=格式错误（out_err 填错误描述）。
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

// 解码 UCX PUT 响应体（UcxPutRsp + errmsg）。
// 返回 0=成功（out_err 填 backend errmsg），-1=格式错误。
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

// 解码 DEL 响应体（DelRsp + errmsg，不含 Message 头）。
// 返回 0=成功（out_err 填 backend errmsg），-1=格式错误。
int DecodeDelResponse(
    const char* buffer,
    std::size_t len,
    DelRsp& out_rsp,
    std::string& out_err);

}  // namespace us3_turbo::proxy
