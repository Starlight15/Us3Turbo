#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/index/upload_index.h"
#include "proxy/src/service/multipart.h"
#include "proxy/src/service/single_put.h"

namespace us3_turbo::proxy {

/**
 * @brief 控制面接口层（Mode B）：唯一 brpc Control 子类，内部委托给服务层。
 *
 * 职责仅：ClosureGuard、proto↔域对象、服务层 int 返回码 → cntl/response。
 * 不做参数校验、不编排——全在 SinglePut / Multipart；
 * 不持 brpc channel——下沉到 BackendGateway / BlockStorage（main 装配注入）。
 *
 * 因 brpc 一个 proto service 只能注册一个 C++ 实例（按 service descriptor
 * full_name 去重），GdsPut/UcxPut/分段 6 个 RPC 必须共处本类；GDS/UCX
 * 代码经服务层各自独立方法保持隔离。
 *
 * 线程安全：构造后成员恒定，handler 可被 brpc 并发调用；下层自带同步。
 */
class ControlPlaneApi final
    : public ::us3_turbo::proxy::Control {
 public:
  ControlPlaneApi(
      std::unique_ptr<SinglePut> single_put,
      std::unique_ptr<Multipart> multipart,
      IUploadIndex* index_for_cleanup);
  ~ControlPlaneApi() override;

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

  void AbortMultipartUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::AbortMultipartUploadRequest* request,
      ::us3_turbo::proxy::AbortMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;

 private:
  // 服务层（main 注入，拥有下层）。
  std::unique_ptr<SinglePut>  single_put_;
  std::unique_ptr<Multipart>  multipart_;

  // 索引层裸指针（main 持有，TTL 清理线程定时 RemoveExpired）。
  IUploadIndex* index_;

  // 后台 TTL 清理线程（析构 join + condition_variable 唤醒）。
  std::thread             cleanup_thread_;
  std::mutex              cleanup_mu_;
  std::condition_variable cleanup_cv_;
  bool                    stop_cleanup_{false};   // cleanup_mu_ 保护
};

}  // namespace us3_turbo::proxy
