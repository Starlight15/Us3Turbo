#pragma once

#include <memory>

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
 * RPC 共处本类（brpc 按 descriptor 去重），GDS/RDMA 经服务层隔离。
 * multipart 会话 TTL 由 MongoDB TTL 索引管理（见 MongoUploadIndex），无需后台
 * 线程；handler 并发安全。 */
class ProxyService final : public Control {
 public:
  ProxyService(std::unique_ptr<SinglePut> single_put, std::unique_ptr<Multipart> multipart,
               std::unique_ptr<GetObject> get_object)
      : single_put_(std::move(single_put)),
        multipart_(std::move(multipart)),
        get_object_(std::move(get_object)) {}

  /* GDS 单块上传，委托 SinglePut。 */
  void GdsPut(google::protobuf::RpcController* cntl, const ClientProxyPutRequest* request,
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

  /* RDMA (libibverbs) 下载，委托 GetObject。 */
  void RdmaGet(google::protobuf::RpcController* cntl, const ClientProxyGetRequest* request,
               GetPathResult* response, google::protobuf::Closure* done) override;

 private:
  // 服务层（main 注入，拥有下层）。
  std::unique_ptr<SinglePut> single_put_;
  std::unique_ptr<Multipart> multipart_;
  std::unique_ptr<GetObject> get_object_;
};

/* 依赖注入装配产物：存储层 + 索引层 + 接口层。
 * 成员析构逆序 service→index→dbgate→ufile_ac。
 */
struct AssembledStack {
  std::unique_ptr<UfileAcClient> ufile_ac;
  std::unique_ptr<DBGateClient> dbgate;
  std::unique_ptr<IUploadIndex> index;
  std::unique_ptr<ProxyService> service;
};

}  // namespace us3_turbo::proxy
