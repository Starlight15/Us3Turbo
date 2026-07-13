#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/index/upload_index.h"
#include "proxy/src/service/get_object.h"
#include "proxy/src/service/multipart.h"
#include "proxy/src/service/single_put.h"

namespace us3_turbo::proxy {

/**
 * @brief Proxy 唯一 brpc Service（Mode B）：实现 Control proto service，内部委托给服务层。
 *
 * 职责仅：ClosureGuard、proto↔域对象、服务层 int 返回值 → cntl/response、
 * Access 日志记录。不做参数校验、不编排——全在 SinglePut / Multipart；
 * 不持 brpc channel——下沉到 UfileAcClient（main 装配注入）。
 *
 * 因 brpc 一个 proto service 只能注册一个 C++ 实例（按 service descriptor
 * full_name 去重），GdsPut/UcxPut/分段 7 个 RPC 必须共处本类；GDS/UCX
 * 代码经服务层各自独立方法保持隔离。
 *
 * 后台 TTL 清理线程：定期扫描索引层删除过期 multipart 会话，见 CleanupThreadMain。
 *
 * 线程安全：构造后成员恒定，handler 可被 brpc 并发调用；下层自带同步。
 */
class ProxyService final
    : public Control {
 public:
  ProxyService(
      std::unique_ptr<SinglePut> single_put,
      std::unique_ptr<Multipart> multipart,
      std::unique_ptr<GetObject> get_object,
      IUploadIndex* index_for_cleanup);
  ~ProxyService() override;

  void GdsPut(
      google::protobuf::RpcController* cntl,
      const ClientProxyPutRequest* request,
      PutPathResult* response,
      google::protobuf::Closure* done) override;

  void UcxPut(
      google::protobuf::RpcController* cntl,
      const ClientProxyPutRequest* request,
      PutPathResult* response,
      google::protobuf::Closure* done) override;

  // ===== 分段上传接口（client → proxy） =====
  void CreateMultipartUpload(
      google::protobuf::RpcController* cntl,
      const CreateMultipartUploadRequest* request,
      CreateMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;

  void UploadPartGds(
      google::protobuf::RpcController* cntl,
      const UploadPartGdsRequest* request,
      UploadPartResponse* response,
      google::protobuf::Closure* done) override;

  void UploadPartUcx(
      google::protobuf::RpcController* cntl,
      const UploadPartUcxRequest* request,
      UploadPartResponse* response,
      google::protobuf::Closure* done) override;

  void CompleteMultipartUpload(
      google::protobuf::RpcController* cntl,
      const CompleteMultipartUploadRequest* request,
      CompleteMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;

  void AbortMultipartUpload(
      google::protobuf::RpcController* cntl,
      const AbortMultipartUploadRequest* request,
      AbortMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;

  // ===== GET 接口（client → proxy） =====
  void StatObject(
      google::protobuf::RpcController* cntl,
      const StatObjectRequest* request,
      StatObjectResponse* response,
      google::protobuf::Closure* done) override;

  void GdsGet(
      google::protobuf::RpcController* cntl,
      const ClientProxyGetRequest* request,
      GetPathResult* response,
      google::protobuf::Closure* done) override;

  void UcxGet(
      google::protobuf::RpcController* cntl,
      const ClientProxyGetRequest* request,
      GetPathResult* response,
      google::protobuf::Closure* done) override;

 private:
  // TTL 清理线程主函数（后台周期扫描，删除过期 multipart 会话）。
  void CleanupThreadMain();

  // 服务层（main 注入，拥有下层）。
  std::unique_ptr<SinglePut>  single_put_;
  std::unique_ptr<Multipart>  multipart_;
  std::unique_ptr<GetObject>  get_object_;

  // 索引层裸指针（main 持有，TTL 清理线程定时 RemoveExpired）。
  IUploadIndex* index_;

  // 后台 TTL 清理线程（析构 join + condition_variable 唤醒）。
  std::thread             cleanup_thread_;
  std::mutex              cleanup_mu_;
  std::condition_variable cleanup_cv_;
  bool                    stop_cleanup_{false};   // cleanup_mu_ 保护
};

}  // namespace us3_turbo::proxy
