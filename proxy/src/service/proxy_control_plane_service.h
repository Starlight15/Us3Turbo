#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/multipart/multipart_put_handler.h"
#include "proxy/src/multipart/session_manager.h"

namespace us3_turbo::proxy {

/**
 * @brief 控制面服务（Mode B）：client 只与本服务交互，统一 PutObject 按
 *        PutDataPath 选择通路，单次 PUT RPC（GdsPut / UcxPut）自带
 *        bucket/key/object_size + 描述符。
 *
 * 链路：client → GdsPut/UcxPut(带描述符)；proxy 内联校验 bucket/key/size +
 *   path/source 匹配后同步转发给 backend 反向 RDMA-READ，backend 返回
 *   PutPathResult 后 proxy 直接回 client。proxy 无状态。
 *
 * 本类同时实现 UcxPut：因 brpc 一个 proto service 只能注册一个 C++ 服务
 * 实例（按 service descriptor full_name 去重），gds 与 ucx 两条链路在
 * proxy 同一进程内无法各注册一个 Control 子类。故 UcxPut 也由本类持有，
 * 转发到同一 backend channel。与"两链路不抽象"原则的折中：UcxPut 与
 * GdsPut 代码完全独立、无共享逻辑，仅在 brpc 注册层面共处一个 service 对象。
 *
 * 分段上传（CreateMultipartUpload / UploadPart{Gds,Ucx} /
 * CompleteMultipartUpload）也由本类持有：与单步接口同属 Control service，
 * 无法另起子类。内存态会话由 SessionManager 管理；part 切分由
 * MultipartPutHandler 经 backend BackendDataPlane_Stub.PutBlock 调 backend。
 *
 * 线程安全：本类无状态（gateway_id_/backend_endpoint_ 构造后只读），
 * backend_channel_/backend_stub_ 构造后恒定不变，所有 RPC handler 可被
 * brpc 并发调用；session_manager_/put_handler_ 内部自带同步。
 */
class ProxyControlPlaneService final
    : public ::us3_turbo::proxy::Control {
 public:
  ProxyControlPlaneService(std::string gateway_id,
                           std::string backend_endpoint,
                           int backend_timeout_ms);
  ~ProxyControlPlaneService() override;

  void GdsPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::ClientProxyPutRequest* request,
      ::us3_turbo::proxy::PutPathResult* response,
      google::protobuf::Closure* done) override;

  void UcxPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::ClientProxyPutRequest* request,
      ::us3_turbo::proxy::PutPathResult* response,
      google::protobuf::Closure* done) override;

  // ===== 分段上传接口（client → proxy） =====
  void CreateMultipartUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::CreateMultipartUploadRequest* request,
      ::us3_turbo::proxy::CreateMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;

  void UploadPartGds(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::UploadPartGdsRequest* request,
      ::us3_turbo::proxy::UploadPartResponse* response,
      google::protobuf::Closure* done) override;

  void UploadPartUcx(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::UploadPartUcxRequest* request,
      ::us3_turbo::proxy::UploadPartResponse* response,
      google::protobuf::Closure* done) override;

  void CompleteMultipartUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::CompleteMultipartUploadRequest* request,
      ::us3_turbo::proxy::CompleteMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;

 private:
  std::string gateway_id_;
  std::string backend_endpoint_;  // backend 数据面地址，用于建 brpc channel
  int         backend_timeout_ms_;

  // 到 backend 的同步转发 channel（Mode B）。构造失败则 stub 为空，
  // GdsPut/UcxPut 以 PROXY_ERR_BACKEND_UNAVAILABLE 拒绝。
  std::shared_ptr<brpc::Channel>                     backend_channel_;
  std::unique_ptr<::us3_turbo::proxy::Control_Stub>   backend_stub_;

  // 到 backend 的 block 级 channel（BackendDataPlane 服务，分段上传用）。
  // 复用同一 backend_endpoint，但 stub 类型不同（BackendDataPlane_Stub）。
  std::unique_ptr<::us3_turbo::proxy::BackendDataPlane_Stub>
      backend_block_stub_;

  // 分段上传会话管理 + part 切分。put_handler_ 持有 backend_block_stub_ 的
  // 裸引用，故声明在 put_handler_ 之前（成员析构逆序：put_handler_ 先析构，
  // 释放对 stub 的引用，再 backend_block_stub_ 析构）。
  SessionManager                          session_manager_;
  std::unique_ptr<MultipartPutHandler>   put_handler_;

  // 后台 TTL 清理线程（detach，进程退出时自然终止）。
  std::thread cleanup_thread_;
  std::atomic<bool> stop_cleanup_{false};
};

}  // namespace us3_turbo::proxy

