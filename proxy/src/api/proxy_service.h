#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/index/dbgate_client.h"
#include "proxy/src/index/upload_index.h"
#include "proxy/src/service/get_object.h"
#include "proxy/src/service/multipart.h"
#include "proxy/src/service/single_put.h"
#include "proxy/src/storage/ufile_ac_client.h"

namespace us3_turbo::proxy {

/* Proxy 唯一 brpc Service（Mode B），实现 Control proto，委托给服务层。
 * 仅负责 ClosureGuard、proto↔域对象转换、int→cntl/response、Access 日志；
 * 7 个 RPC 共处本类（brpc 按 descriptor 去重），GDS/UCX 经服务层隔离。
 * 后台 TTL 清理线程定期删除过期 multipart 会话；构造后成员恒定，handler
 * 并发安全。 */
class ProxyService final : public Control {
 public:
  ProxyService(std::unique_ptr<SinglePut> single_put, std::unique_ptr<Multipart> multipart,
               std::unique_ptr<GetObject> get_object, IUploadIndex* index_for_cleanup)
      : single_put_(std::move(single_put)),
        multipart_(std::move(multipart)),
        get_object_(std::move(get_object)),
        index_(index_for_cleanup) {
    // 启动后台 TTL 清理线程，周期扫描删除过期 multipart 会话
    cleanup_thread_ = std::thread([this]() { CleanupThreadMain(); });
  }

  ~ProxyService() override {
    {
      std::lock_guard lock(cleanup_mu_);
      stop_cleanup_ = true;
    }
    cleanup_cv_.notify_all();
    if (cleanup_thread_.joinable()) cleanup_thread_.join();
  }

  /* GDS 单块上传，委托 SinglePut。 */
  void GdsPut(google::protobuf::RpcController* cntl, const ClientProxyPutRequest* request,
              PutPathResult* response, google::protobuf::Closure* done) override;

  /* UCX 单块上传，委托 SinglePut。 */
  void UcxPut(google::protobuf::RpcController* cntl, const ClientProxyPutRequest* request,
              PutPathResult* response, google::protobuf::Closure* done) override;

  /* RDMA 单块上传，委托 SinglePut。 */
  void RdmaPut(google::protobuf::RpcController* cntl, const ClientProxyPutRequest* request,
               PutPathResult* response, google::protobuf::Closure* done) override;

  // ===== 分段上传接口（client → proxy） =====
  /* 创建分段上传会话，委托 Multipart。 */
  void CreateMultipartUpload(google::protobuf::RpcController* cntl,
                             const CreateMultipartUploadRequest* request,
                             CreateMultipartUploadResponse* response,
                             google::protobuf::Closure* done) override;

  /* GDS 分段上传 part，委托 Multipart。 */
  void UploadPartGds(google::protobuf::RpcController* cntl, const UploadPartGdsRequest* request,
                     UploadPartResponse* response, google::protobuf::Closure* done) override;

  /* UCX 分段上传 part，委托 Multipart。 */
  void UploadPartUcx(google::protobuf::RpcController* cntl, const UploadPartUcxRequest* request,
                     UploadPartResponse* response, google::protobuf::Closure* done) override;

  /* RDMA (libibverbs) 分段上传 part，委托 Multipart。 */
  void UploadPartRdma(google::protobuf::RpcController* cntl, const UploadPartRdmaRequest* request,
                      UploadPartResponse* response, google::protobuf::Closure* done) override;

  /* 完成分段上传，委托 Multipart。 */
  void CompleteMultipartUpload(google::protobuf::RpcController* cntl,
                               const CompleteMultipartUploadRequest* request,
                               CompleteMultipartUploadResponse* response,
                               google::protobuf::Closure* done) override;

  /* 取消分段上传，委托 Multipart。 */
  void AbortMultipartUpload(google::protobuf::RpcController* cntl,
                            const AbortMultipartUploadRequest* request,
                            AbortMultipartUploadResponse* response,
                            google::protobuf::Closure* done) override;

  // ===== GET 接口（client → proxy） =====
  /* 查询对象元数据，委托 GetObject。 */
  void StatObject(google::protobuf::RpcController* cntl, const StatObjectRequest* request,
                  StatObjectResponse* response, google::protobuf::Closure* done) override;

  /* GDS 下载，委托 GetObject。 */
  void GdsGet(google::protobuf::RpcController* cntl, const ClientProxyGetRequest* request,
              GetPathResult* response, google::protobuf::Closure* done) override;

  /* UCX 下载，委托 GetObject。 */
  void UcxGet(google::protobuf::RpcController* cntl, const ClientProxyGetRequest* request,
              GetPathResult* response, google::protobuf::Closure* done) override;

 private:
  /* TTL 清理线程主函数，周期扫描删除过期 multipart 会话。 */
  void CleanupThreadMain();

  // 服务层（main 注入，拥有下层）。
  std::unique_ptr<SinglePut> single_put_;
  std::unique_ptr<Multipart> multipart_;
  std::unique_ptr<GetObject> get_object_;

  // 索引层（main 持有，TTL 清理线程定时 RemoveExpired）
  IUploadIndex* index_;

  // 后台 TTL 清理线程
  std::thread cleanup_thread_;
  std::mutex cleanup_mu_;
  std::condition_variable cleanup_cv_;
  bool stop_cleanup_{false};  // cleanup_mu_ 保护
};

/* 依赖注入装配产物：存储层 + 索引层 + 接口层。
 * 成员析构逆序 service→index→dbgate→ufile_ac，保证 TTL 清理先完成再释放下层。
 */
struct AssembledStack {
  std::unique_ptr<UfileAcClient> ufile_ac;
  std::unique_ptr<DBGateClient> dbgate;
  std::unique_ptr<IUploadIndex> index;
  std::unique_ptr<ProxyService> service;
};

}  // namespace us3_turbo::proxy
