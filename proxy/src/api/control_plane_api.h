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
  // 服务层
  std::unique_ptr<SinglePut>  single_put_;
  std::unique_ptr<Multipart>  multipart_;

  // 索引层裸指针
  IUploadIndex* index_;

  // 后台 TTL 清理线程
  std::thread             cleanup_thread_;
  std::mutex              cleanup_mu_;
  std::condition_variable cleanup_cv_;
  bool                    stop_cleanup_{false};
};

}  // namespace us3_turbo::proxy
