#pragma once

// backend_protocol.h — proxy → backend (ufile-ac) 自定义二进制协议定义。
//
// 对齐 /mnt/us3_test/ld/ufile-ac/message.h。ufile-ac 服务端按固定布局解析，
// 各 struct __attribute__((packed))，无填充。
//
// 字节序（实测 ufile-ac 服务端代码）：
// - Message.msgSize_   是网络字节序（BigEndian），发送须 htonl，接收无须 ntohl
//   （服务端按 readable 字节读，不解析此字段值）。
// - Message 其余字段 + GdsPutReq/Rsp + UcxPutReq/Rsp 全部字段：主机序
//   （x86 = LittleEndian），直接赋值，不转换。
// - 服务端用 bkdrHash(key) 内部 hash key 字符串，客户端发原始 key（≤48 字节，
//   KEY_MAX_LENGTH）。
// - GdsPutRsp.etagLen_ 服务端恒为 0（memset，从不填）→ backend 不返回 etag，
//   proxy 须自行合成（见 backend_gateway.cpp）。

#include <cstdint>

namespace us3_turbo::proxy {

// 协议常量（对齐 ufile-ac/message.h）
constexpr std::uint32_t MESSAGE_MAGIC_NUMBER   = 0x0a0a0a0a;
constexpr std::uint32_t MESSAGE_VERSION_NUMBER = 0x01;

// 消息类型（对齐 ufile-ac message.h MessageType）
enum MessageType : std::uint32_t {
  OSD_GDS_PUT_REQ = 21,
  OSD_GDS_PUT_RSP = 22,
  OSD_UCX_PUT_REQ = 25,
  OSD_UCX_PUT_RSP = 26,
};

// 通用消息头（sizeof = 52，packed）。body_ 为柔性数组，不计入 sizeof。
struct Message {
  std::uint32_t msgSize_;         // 消息总长度（不含自己的 4 字节），BigEndian
  std::uint32_t magic_;           // MESSAGE_MAGIC_NUMBER
  std::uint32_t version_;         // MESSAGE_VERSION_NUMBER
  std::uint32_t type_;            // MessageType
  std::uint32_t flowno_;          // flow number，原样返回
  std::uint64_t sessionIdLow_;
  std::uint64_t sessionIdHigh_;
  std::uint32_t setid_;           // 服务端校验 == g_setid
  std::uint64_t payload_;         // 业务信息（ec 存 replica_nth）
  std::uint32_t bodyLen_;         // body_ 长度
  char         body_[0];          // 柔性数组
} __attribute__((packed));

// GDS PUT 请求（sizeof = 52，packed）+ 变长 data_（key bytes + rdma_token bytes）。
struct GdsPutReq {
  std::uint32_t keyLen_;          // key/objid 长度，≤ KEY_MAX_LENGTH(48)
  std::uint32_t tokenLen_;        // rdma_token 长度
  std::uint64_t dataLen_;         // 数据长度，≤ MAX_VALUE_LENGTH(16M)
  std::uint64_t gpuOffset_;       // 当前 chunk 在 GPU buffer 偏移，单对象可为 0
  std::uint64_t requestId_;       // proxy 生成，日志/排障
  std::uint64_t sessionIdLow_;
  std::uint64_t sessionIdHigh_;
  std::uint32_t flags_;           // 预留
  char         data_[0];          // key bytes + rdma_token bytes
} __attribute__((packed));

// GDS PUT 响应（sizeof = 24，packed）+ 变长 data_（etag bytes + errmsg bytes）。
// 注：ufile-ac 服务端 memset 后只填 retcode_/crc32c_/bytesWritten_/errMsgLen_，
// etagLen_ 恒为 0（backend 不返回 etag）。
struct GdsPutRsp {
  std::int32_t  retcode_;
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t etagLen_;         // backend 恒为 0
  std::uint32_t errMsgLen_;
  char         data_[0];          // etag bytes + errmsg bytes
} __attribute__((packed));

// UCX PUT 请求（sizeof = 68，packed）+ 变长 data_（key + client_ucx_addr + packed_rkey）。
struct UcxPutReq {
  std::uint32_t keyLen_;
  std::uint32_t addrLen_;         // client_ucx_addr 长度
  std::uint32_t rkeyLen_;         // packed_rkey 长度
  std::uint32_t reserved0_;
  std::uint64_t dataLen_;
  std::uint64_t remoteAddr_;      // client source buffer 虚拟地址
  std::uint64_t sourceOffset_;    // 当前 chunk 在 source buffer 偏移
  std::uint64_t requestId_;
  std::uint64_t sessionIdLow_;
  std::uint64_t sessionIdHigh_;
  std::uint32_t flags_;
  char         data_[0];          // key + client_ucx_addr + packed_rkey
} __attribute__((packed));

// UCX PUT 响应（sizeof = 20，packed）+ 变长 data_（errmsg bytes）。
struct UcxPutRsp {
  std::int32_t  retcode_;
  std::uint32_t crc32c_;
  std::uint64_t bytesWritten_;
  std::uint32_t errMsgLen_;
  char         data_[0];          // errmsg bytes
} __attribute__((packed));

}  // namespace us3_turbo::proxy
