#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"

namespace us3_turbo::proxy {

/**
 * @brief 控制面服务（Mode B）：client 只与本服务交互，单次 PUT RPC（GdsPut /
 *        RdmaPut）自带 bucket/key/chunk_size + 描述符。
 *
 * 链路：client → GdsPut/RdmaPut(带描述符)；proxy 内联校验 bucket/key/size 后
 *   同步转发给 backend 反向 RDMA-READ，backend 返回 etag/crc/bytes 后 proxy
 *   直接回 client。proxy 无状态（不再有 session 记录）。
 *
 * 本类同时实现 RdmaPut：因 brpc 一个 proto service 只能注册一个 C++ 服务
 * 实例（按 service descriptor full_name 去重），gds 与 rdma 两条链路在
 * proxy 同一进程内无法各注册一个 Control 子类。故 RdmaPut 也由本类持有，
 * 转发到同一 backend channel。与"两链路不抽象"原则的折中：RdmaPut 与
 * GdsPut 代码完全独立、无共享逻辑，仅在 brpc 注册层面共处一个 service 对象。
 *
 * 线程安全：本类无状态（gateway_id_/backend_endpoint_ 构造后只读），
 * backend_channel_/backend_stub_ 构造后恒定不变，所有 RPC handler 可被
 * brpc 并发调用。
 */
class ProxyControlPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  ProxyControlPlaneService(std::string gateway_id,
                           std::string backend_endpoint,
                           int backend_timeout_ms);

  void GdsPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::GdsChunkRequest* request,
      ::us3_turbo::proxy::GdsChunkResponse* response,
      google::protobuf::Closure* done) override;

  void RdmaPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::RdmaChunkRequest* request,
      ::us3_turbo::proxy::RdmaChunkResponse* response,
      google::protobuf::Closure* done) override;

 private:
  std::string gateway_id_;
  std::string backend_endpoint_;  // backend 数据面地址，用于建 brpc channel
  int         backend_timeout_ms_;

  // 到 backend 的同步转发 channel（Mode B）。构造失败则 stub 为空，
  // GdsPut/RdmaPut 以 PROXY_ERR_BACKEND_UNAVAILABLE 拒绝。
  std::shared_ptr<brpc::Channel>                     backend_channel_;
  std::unique_ptr<::us3_turbo::proxy::Control_Stub>   backend_stub_;
};

}  // namespace us3_turbo::proxy
