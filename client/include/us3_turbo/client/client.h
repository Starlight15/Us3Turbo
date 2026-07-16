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
class UcxPutChannel;
class UcxGetChannel;
class PutChannel;
class GdsMemoryManager;
class UcxMemoryManager;

/**
 * @brief 对象存储 client。
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

  /** @brief 初始化 brpc 与 GDS/UCX channel,幂等。 */
  [[nodiscard]] bool Initialize();

  /**  @brief 统一 PUT 入口:按 request.path 选 GDS/UCX 通路。*/
  [[nodiscard]] bool PutObject(const ClientProxyPutRequest& request,
                               ConstBufferView buffer,
                               ClientProxyPutResponse& response) const;

  // ===== 分段上传接口 =====

  using CompletedMultipart = ProxyRpc::CompletedMultipart;

  /** @brief client 侧 part 信息（part_number + etag），用于 Complete 校验。 */
  struct PartInfo {
    std::uint32_t part_number{0};
    std::string etag;
  };

  /** @brief 初始化分段上传，返回 upload_id。path 锁定整条会话通路。 */
  [[nodiscard]] bool CreateMultipartUpload(const std::string& bucket,
                                           const std::string& key, PutDataPath path,
                                           std::string& out_upload_id,
                                           std::string& out_error) const;

  /** @brief GDS 路径上传单个 part：为本 part 独立注册 RDMA token。 */
  [[nodiscard]] bool UploadPartGds(const std::string& upload_id,
                                   std::uint32_t part_number, ConstBufferView buffer,
                                   std::string& out_etag, std::string& out_error) const;

  /** @brief UCX 路径上传单个 part：为本 part 独立注册 descriptor。 */
  [[nodiscard]] bool UploadPartUcx(const std::string& upload_id,
                                   std::uint32_t part_number, ConstBufferView buffer,
                                   std::string& out_etag, std::string& out_error) const;

  /** @brief 完成分段上传，返回最终 object_id/etag/size。 */
  [[nodiscard]] bool CompleteMultipartUpload(const std::string& upload_id,
                                             const std::vector<PartInfo>& parts,
                                             CompletedMultipart& out) const;

  /** @brief 终止分段上传，释放 proxy 会话（幂等）。 */
  [[nodiscard]] bool AbortMultipartUpload(const std::string& upload_id,
                                          std::string& out_error) const;

  // ===== GET 接口 =====

  /** @brief 查对象布局（GetObject 第一步），返回 object_size 供调用方分配buffer。 */
  [[nodiscard]] bool StatObject(const std::string& bucket, const std::string& key,
                                std::uint64_t& out_object_size,
                                std::string& out_error) const;

  /**  @brief GDS 通路 GET：buffer 须已按 StatObject 返回的 size 分配。 */
  [[nodiscard]] bool GetObjectGds(const std::string& bucket, const std::string& key,
                                  MutableBufferView buffer, GetPathResult& result) const;

  /** @brief UCX 通路 GET：buffer 须已按 StatObject 返回的 size 分配（host内存）。 */
  [[nodiscard]] bool GetObjectUcx(const std::string& bucket, const std::string& key,
                                  MutableBufferView buffer, GetPathResult& result) const;

 private:
  ClientOptions options_;
  std::unique_ptr<ProxyRpc> proxy_;
  std::unique_ptr<GdsPutChannel> gds_channel_;
  std::unique_ptr<GdsGetChannel> gds_get_channel_;
  std::unique_ptr<UcxPutChannel> ucx_channel_;
  std::unique_ptr<UcxGetChannel> ucx_get_channel_;
  bool initialized_{false};

  [[nodiscard]] bool ValidatePutPath(const ClientProxyPutRequest& req) const;

  [[nodiscard]] PutChannel* SelectChannel(PutDataPath path) const noexcept;

  // 返回 client 进程内的 GDS/UCX manager 单例（Initialize 时已确保可用）。
  [[nodiscard]] GdsMemoryManager* GdsManager() const;
  [[nodiscard]] UcxMemoryManager* UcxManager() const;
};

}  // namespace us3_turbo::client
