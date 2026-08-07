#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "client/src/common/request.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class GdsPutChannel;
class GdsGetChannel;
class RdmaPutChannel;
class RdmaGetChannel;
class GdsMemoryManager;
class RdmaMemoryManager;

/*
 * 对象存储 client：GDS (CUDA cuObj) / RDMA (libibverbs) 双通路。
 * 单步 PUT、分段上传、GET 接口。
 */
class Client {
 public:
  explicit Client(ClientOptions options);

  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** @brief 关闭client。 */
  void Shutdown();

  /** @brief 是否完成初始化。 */
  [[nodiscard]] bool initialized() const;

  /** @brief 初始化 brpc 与 GDS/RDMA channel,幂等。 */
  [[nodiscard]] bool Initialize();

  /** @brief GDS 单步 PUT：device 显存，走 cuObj RDMA 链路。*/
  [[nodiscard]] bool PutObjectGds(const ClientProxyPutRequest& req, ConstBufferView buffer,
                                   ClientProxyPutResponse& resp) const;

  /** @brief RDMA 单步 PUT：host 内存，走 libibverbs RDMA CM 链路。*/
  [[nodiscard]] bool PutObjectRdma(const ClientProxyPutRequest& req, ConstBufferView buffer,
                                    ClientProxyPutResponse& resp) const;

  // ===== 分段上传接口 =====

  using CompletedMultipart = ProxyRpc::CompletedMultipart;

  /** @brief client 侧 part 信息（part_number + etag），用于 Complete 校验。 */
  struct PartInfo {
    std::uint32_t part_number{0};
    std::string etag;
  };

  /** @brief 初始化分段上传，返回 upload_id 与 proxy 生成的 trace_id。
   * path 锁定整条会话通路。trace_id 仅代表本笔 Create RPC(后续 UploadPart
   * 等 RPC 各自由 proxy 生成新 trace_id,在各自响应里回 client)。 */
  [[nodiscard]] bool CreateMultipartUpload(const std::string& bucket, const std::string& key,
                                           PutDataPath path, std::string& out_upload_id,
                                           std::string& out_trace_id,
                                           std::string& out_error) const;

  /** @brief GDS 路径上传单个 part：为本 part 独立注册 RDMA token。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool UploadPartGds(const std::string& upload_id,
                                   std::uint32_t part_number,
                                   ConstBufferView buffer, std::string& out_etag,
                                   std::string& out_error) const;

  /** @brief RDMA (libibverbs) 路径上传单个 part：为本 part 独立注册 MR + 编码 token。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool UploadPartRdma(const std::string& upload_id,
                                    std::uint32_t part_number,
                                    ConstBufferView buffer, std::string& out_etag,
                                    std::string& out_error) const;

  /** @brief 完成分段上传，返回最终 object_id/etag/size。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool CompleteMultipartUpload(const std::string& upload_id,
                                             const std::vector<PartInfo>& parts,
                                             CompletedMultipart& out) const;

  /** @brief 终止分段上传，释放 proxy 会话（幂等）。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool AbortMultipartUpload(const std::string& upload_id,
                                          std::string& out_error) const;

  // ===== GET 接口 =====

  /** @brief 查对象布局（GetObject 第一步），返回 object_size 与 proxy
   * 生成的 trace_id。trace_id 仅代表本笔 Stat RPC(后续 GetObject 由 proxy
   * 生成新 trace_id,在响应里回 client)。 */
  [[nodiscard]] bool StatObject(const std::string& bucket, const std::string& key,
                                std::uint64_t& out_object_size, std::string& out_trace_id,
                                std::string& out_error) const;

  /**  @brief GDS 通路 GET：buffer 须已按 StatObject 返回的 size 分配。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool GetObjectGds(const std::string& bucket, const std::string& key,
                                  MutableBufferView buffer, GetPathResult& res) const;

  /** @brief RDMA (libibverbs) 通路 GET：buffer 须已按 StatObject 返回的 size 分配。
   * backend 从 NVMe 读数据后 RDMA WRITE 推到 client host buffer。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool GetObjectRdma(const std::string& bucket, const std::string& key,
                                   MutableBufferView buffer, GetPathResult& res) const;

 private:
  ClientOptions opts_;
  std::unique_ptr<ProxyRpc> proxy_;
  std::unique_ptr<GdsPutChannel> gds_channel_;
  std::unique_ptr<GdsGetChannel> gds_get_channel_;
  std::unique_ptr<RdmaPutChannel> rdma_channel_;
  std::unique_ptr<RdmaGetChannel> rdma_get_channel_;
  bool initialized_{false};

  // 返回 client 进程内的 GDS/RDMA manager 单例（Initialize 时已确保可用）。
  [[nodiscard]] GdsMemoryManager* GdsManager() const;

  [[nodiscard]] RdmaMemoryManager* RdmaManager() const;
};

}  // namespace us3_turbo::client
