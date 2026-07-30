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

/* 协议常量（对齐 ufile-ac/message.h） */
constexpr std::uint32_t MESSAGE_MAGIC_NUMBER = 0x0a0a0a0a;
constexpr std::uint32_t MESSAGE_VERSION_NUMBER = 0x01;
constexpr std::uint32_t KEY_MAX_LENGTH = 48;
constexpr std::uint64_t MAX_VALUE_LENGTH = 16777216ULL;  // 16 MiB

/* 消息类型（对齐 ufile-ac message.h MessageType，仅列本仓用到的） */
enum MessageType : std::uint32_t {
  OSD_DEL_REQ = 7,
  OSD_DEL_RSP = 8,
  OSD_GDS_PUT_REQ = 21,
  OSD_GDS_PUT_RSP = 22,
  OSD_GDS_GET_REQ = 23,
  OSD_GDS_GET_RSP = 24,
  OSD_RDMA_PUT_REQ = 29,
  OSD_RDMA_PUT_RSP = 30,
  OSD_RDMA_GET_REQ = 31,
  OSD_RDMA_GET_RSP = 32,
};

/*
 * 通用消息头（sizeof = 52，packed）。body_ 柔性数组不计入 sizeof。
 *   data_   = body_（变长，按 type 解释为各 Req/Rsp）
 *   bodyLen_ = body_ 长度（主机序）
 */
struct Message {
  std::uint32_t msgSize_;  // 消息总长度（不含自身 4 字节），BigEndian
  std::uint32_t magic_;
  std::uint32_t version_;
  std::uint32_t type_;
  std::uint32_t flowno_;         // 原样返回，预留填 0
  std::uint64_t sessionIdLow_;   // 服务端统计用，填自增值
  std::uint64_t sessionIdHigh_;  // 预留
  std::uint32_t setid_;          // 必填，== backend g_setid
  std::uint64_t payload_;        // 业务信息，预留填 0
  std::uint32_t bodyLen_;
  char body_[0];
} __attribute__((packed));

/*
 * GDS PUT 请求（sizeof = 52）+ 变长 data_。
 *   data_   = key bytes || rdma_token bytes
 *   bodyLen_ = GDS_PUT_REQ_SIZE + keyLen_ + tokenLen_
 */
struct GdsPutReq {
  std::uint32_t keyLen_;
  std::uint32_t tokenLen_;
  std::uint64_t dataLen_;
  std::uint64_t gpuOffset_;      // chunk 在 GPU buffer 偏移
  std::uint64_t requestId_;      // 预留填 0
  std::uint64_t sessionIdLow_;   // 预留填 0
  std::uint64_t sessionIdHigh_;  // 预留填 0
  std::uint32_t flags_;          // 预留填 0
  char data_[0];                 // key + rdma_token
} __attribute__((packed));

/*
 * GDS PUT 响应（sizeof = 24）+ 变长 data_。
 *   data_   = etag bytes(etagLen_) || errmsg bytes(errMsgLen_)
 *   注：backend etagLen_ 恒 0（不返回 etag）。
 */
struct GdsPutRsp {
  std::int32_t retcode_;  // 0=成功
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t etagLen_;  // backend 恒 0
  std::uint32_t errMsgLen_;
  char data_[0];  // etag + errmsg
} __attribute__((packed));

/*
 * GDS GET 请求（sizeof = 60）+ 变长 data_。布局与 GdsPutReq 不同：
 * tokenLen_ 后多 readOffset_，字段顺序须与 backend message.h 一致。
 */
struct GdsGetReq {
  std::uint32_t keyLen_;
  std::uint32_t tokenLen_;
  std::uint64_t readOffset_;  // 对象内读偏移，本阶段恒 0（整对象读）
  std::uint64_t dataLen_;
  std::uint64_t gpuOffset_;      // chunk 写入 client GPU buffer 的偏移
  std::uint64_t requestId_;      // 预留填 0
  std::uint64_t sessionIdLow_;   // 预留填 0
  std::uint64_t sessionIdHigh_;  // 预留填 0
  std::uint32_t flags_;          // 预留填 0
  char data_[0];                 // key + rdma_token
} __attribute__((packed));

/*
 * GDS GET 响应（sizeof = 20）+ 变长 data_（errmsg bytes，无 etag）。
 */
struct GdsGetRsp {
  std::int32_t retcode_;  // 0=成功
  std::uint32_t crc32c_;
  std::uint64_t bytesRead_;
  std::uint32_t errMsgLen_;
  char data_[0];  // errmsg bytes
} __attribute__((packed));

/*
 * DEL 请求（sizeof = 12）+ 变长 key_。删除一个对象（block）。
 *   bodyLen_ = DEL_REQ_SIZE + keyLen_
 *   服务端支持 key_ 内多 key 逗号分隔走 DelBatch；本仓单 key 走 Del。
 */
struct DelReq {
  std::uint32_t keyLen_;
  std::uint64_t reserve_;  // 保留填 0
  char key_[0];
} __attribute__((packed));

/*
 * DEL 响应（sizeof = 8）+ 变长 data_（errmsg bytes）。
 *   retcode_ KEY_NOT_FOUND 在清理场景可接受，调用方按 WARN 处理。
 */
struct DelRsp {
  std::int32_t retcode_;  // 0=成功
  std::uint32_t errMsgLen_;
  char data_[0];  // errmsg bytes
} __attribute__((packed));

/*
 * RDMA PUT 请求（sizeof=60）+ 变长 data_。对齐 ufile-ac message.h RdmaPutReq。
 *   data_   = key bytes || token bytes
 *   bodyLen_ = RDMA_PUT_REQ_SIZE + keyLen_ + tokenLen_
 */
struct RdmaPutReq {
  std::uint32_t keyLen_;
  std::uint32_t tokenLen_;
  std::uint64_t dataLen_;
  std::uint64_t sourceOffset_;
  std::uint64_t requestId_;
  std::uint64_t sessionIdLow_;
  std::uint64_t sessionIdHigh_;
  std::uint32_t flags_;
  char data_[0];
} __attribute__((packed));

/*
 * RDMA PUT 响应（sizeof=20）+ 变长 data_（errmsg bytes）。
 */
struct RdmaPutRsp {
  std::int32_t retcode_;
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t errMsgLen_;
  char data_[0];
} __attribute__((packed));

/*
 * RDMA GET 请求（sizeof=68）+ 变长 data_。对齐 ufile-ac message.h RdmaGetReq。
 * backend 从 NVMe 读数据后 RDMA WRITE 到 client buffer。
 */
struct RdmaGetReq {
  std::uint32_t keyLen_;
  std::uint32_t tokenLen_;
  std::uint64_t readOffset_;     // 对象内读偏移，本阶段恒 0（整对象读）
  std::uint64_t dataLen_;        // 本次读取长度
  std::uint64_t destOffset_;     // 写入 client buffer 的偏移
  std::uint64_t requestId_;      // proxy 生成，用于日志/排障
  std::uint64_t sessionIdLow_;   // 用于关联 proxy session
  std::uint64_t sessionIdHigh_;
  std::uint32_t flags_;          // 预留
  char data_[0];                 // key bytes + token bytes
} __attribute__((packed));

/*
 * RDMA GET 响应（sizeof=20）+ 变长 data_（errmsg bytes）。
 */
struct RdmaGetRsp {
  std::int32_t retcode_;
  std::uint32_t crc32c_;
  std::uint64_t bytesRead_;
  std::uint32_t errMsgLen_;
  char data_[0];  // errmsg bytes
} __attribute__((packed));

/* 尺寸常量（用 sizeof，避免硬编码笔误） */
constexpr std::size_t MESSAGE_HEAD_SIZE = sizeof(Message);
constexpr std::size_t GDS_PUT_REQ_SIZE = sizeof(GdsPutReq);
constexpr std::size_t GDS_PUT_RSP_SIZE = sizeof(GdsPutRsp);
constexpr std::size_t GDS_GET_REQ_SIZE = sizeof(GdsGetReq);
constexpr std::size_t GDS_GET_RSP_SIZE = sizeof(GdsGetRsp);
constexpr std::size_t DEL_REQ_SIZE = sizeof(DelReq);
constexpr std::size_t DEL_RSP_SIZE = sizeof(DelRsp);
constexpr std::size_t RDMA_PUT_REQ_SIZE = sizeof(RdmaPutReq);
constexpr std::size_t RDMA_PUT_RSP_SIZE = sizeof(RdmaPutRsp);
constexpr std::size_t RDMA_GET_REQ_SIZE = sizeof(RdmaGetReq);
constexpr std::size_t RDMA_GET_RSP_SIZE = sizeof(RdmaGetRsp);

/* 编解码函数 */

/* 编码 GDS PUT 请求，返回总字节数。
 * 布局: Message(52) + GdsPutReq(52) + key + rdma_token */
std::size_t EncodeGdsPutRequest(const std::string& key, const std::string& rdma_token,
                                std::uint64_t gpu_offset, std::uint64_t data_len,
                                std::uint32_t setid, std::uint64_t session_id,
                                std::vector<char>& out_buffer);

/* 解码 GDS PUT 响应体（不含 Message 头）。
 * 返回 0=成功，-1=格式错误。 */
int DecodeGdsPutResponse(const char* buffer, std::size_t len, GdsPutRsp& out_rsp,
                         std::string& out_err);


/* 编码 RDMA PUT 请求，返回总字节数。
 * 布局: Message(52) + RdmaPutReq(60) + key + token */
std::size_t EncodeRdmaPutRequest(const std::string& key, const std::string& token,
                                 std::uint64_t source_offset, std::uint64_t data_len,
                                 std::uint32_t setid, std::uint64_t session_id,
                                 std::vector<char>& out_buffer);

/* 解码 RDMA PUT 响应体（不含 Message 头）。返回 0=成功，-1=格式错误。 */
int DecodeRdmaPutResponse(const char* buffer, std::size_t len, RdmaPutRsp& out_rsp,
                           std::string& out_err);

/* RDMA GET */

/* 编码 RDMA GET 请求，返回总字节数。
 * 布局: Message(52) + RdmaGetReq(68) + key + token */
std::size_t EncodeRdmaGetRequest(const std::string& key, const std::string& token,
                                 std::uint64_t read_offset, std::uint64_t dest_offset,
                                 std::uint64_t data_len, std::uint32_t setid,
                                 std::uint64_t session_id, std::uint64_t request_id,
                                 std::vector<char>& out_buffer);

/* 解码 RDMA GET 响应体（不含 Message 头）。返回 0=成功，-1=格式错误。 */
int DecodeRdmaGetResponse(const char* buffer, std::size_t len, RdmaGetRsp& out_rsp,
                           std::string& out_err);

/* 编码 DEL 请求，返回总字节数。
 * 布局: Message(52) + DelReq(12) + key */
std::size_t EncodeDelRequest(const std::string& key, std::uint32_t setid, std::uint64_t session_id,
                             std::vector<char>& out_buffer);

/* 解码 DEL 响应体（不含 Message 头）。返回 0=成功，-1=格式错误。 */
int DecodeDelResponse(const char* buffer, std::size_t len, DelRsp& out_rsp, std::string& out_err);

/* GDS GET */

/* 编码 GDS GET 请求，返回总字节数。
 * 布局: Message(52) + GdsGetReq(60) + key + rdma_token */
std::size_t EncodeGdsGetRequest(const std::string& key, const std::string& rdma_token,
                                std::uint64_t read_offset, std::uint64_t gpu_offset,
                                std::uint64_t data_len, std::uint32_t setid,
                                std::uint64_t session_id, std::uint64_t request_id,
                                std::vector<char>& out_buffer);

/* 解码 GDS GET 响应体（不含 Message 头）。返回 0=成功，-1=格式错误。 */
int DecodeGdsGetResponse(const char* buffer, std::size_t len, GdsGetRsp& out_rsp,
                         std::string& out_err);

}  // namespace us3_turbo::proxy
