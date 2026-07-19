#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "client/src/common/request.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief proxy 控制面 RPC client:GdsPut / UcxPut 共用一条 brpc channel。
 */
class ProxyRpc {
 public:
  ProxyRpc(const std::string& endpoint, std::chrono::milliseconds rpc_timeout)
      : rpc_timeout_(rpc_timeout) {
    if (endpoint.empty()) {
      init_error_ = "proxy endpoint must not be empty";
      return;
    }
    channel_ = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions co;
    co.protocol = "baidu_std";
    co.connect_timeout_ms = static_cast<int>(rpc_timeout.count());
    co.timeout_ms = static_cast<int>(rpc_timeout.count());
    co.max_retry = 2;
    std::string trimmed = endpoint;
    while (!trimmed.empty() && trimmed.back() == '/') {
      trimmed.pop_back();
    }
    if (channel_->Init(trimmed.c_str(), nullptr, &co) != 0) {
      init_error_ = "Failed to initialize brpc channel: " + endpoint;
      channel_.reset();
      return;
    }
    stub_ = std::make_unique<us3_turbo::proxy::Control_Stub>(channel_.get());
  }

  ProxyRpc(const ProxyRpc&) = delete;
  ProxyRpc& operator=(const ProxyRpc&) = delete;
  ProxyRpc(ProxyRpc&&) = delete;
  ProxyRpc& operator=(ProxyRpc&&) = delete;

  ~ProxyRpc() = default;

  /** @brief RPC 层是否就绪(channel + stub 初始化成功)。 */
  [[nodiscard]] bool ok() const { return channel_ != nullptr && stub_ != nullptr; }

  /** @brief 初始化失败时的错误描述(ok() 为 true 时为空)。 */
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

  /** @brief GDS 通路:cuObj RDMA token 随 RPC 透传,backend 反向 RDMA-READ。 */
  [[nodiscard]] bool GdsPut(std::string_view req_id, const std::string& bucket,
                            const std::string& key, std::uint64_t object_size,
                            const GdsDataSource& gds_source, PutPathResult& res) const;

  /** @brief UCX 通路:描述符随 RPC 透传,backend ucp_get_nbx 反向拉取。 */
  [[nodiscard]] bool UcxPut(std::string_view req_id, const std::string& bucket,
                            const std::string& key, std::uint64_t object_size,
                            const UcxDataSource& ucx_source, PutPathResult& res) const;

  /** @brief RDMA (libibverbs) 通路:token 随 RPC 透传,backend RDMA CM 反向连接后
   * ibv_post_send(RDMA_READ)。 */
  [[nodiscard]] bool RdmaPut(std::string_view req_id, const std::string& bucket,
                             const std::string& key, std::uint64_t object_size,
                             const RdmaDataSource& rdma_source, PutPathResult& res) const;

  // ===== 分段上传接口（client → proxy）=====

  /** @brief 初始化分段上传，返回 upload_id。 */
  [[nodiscard]] bool CreateMultipartUpload(std::string_view req_id, const std::string& bucket,
                                           const std::string& key,
                                           ::us3_turbo::proxy::PutDataPath path,
                                           std::string& out_upload_id,
                                           std::string& out_error) const;

  /** @brief GDS 路径上传单个 part：rdma_token 随 RPC 透传。 */
  [[nodiscard]] bool UploadPartGds(std::string_view req_id, const std::string& upload_id,
                                   std::uint32_t part_number, std::uint64_t part_size,
                                   const std::string& rdma_token, PutPathResult& res) const;

  /** @brief UCX 路径上传单个 part：描述符随 RPC 透传。 */
  [[nodiscard]] bool UploadPartUcx(std::string_view req_id, const std::string& upload_id,
                                   std::uint32_t part_number, std::uint64_t part_size,
                                   std::uint64_t remote_addr, const std::string& packed_rkey,
                                   const std::string& client_ucx_addr, PutPathResult& res) const;

  /** @brief RDMA (libibverbs) 路径上传单个 part：rdma_token 随 RPC 透传。 */
  [[nodiscard]] bool UploadPartRdma(std::string_view req_id, const std::string& upload_id,
                                    std::uint32_t part_number, std::uint64_t part_size,
                                    const std::string& rdma_token, PutPathResult& res) const;

  /** @brief 完成分段上传，返回最终 object_id/etag/size。 */
  struct CompletedMultipart {
    bool ok{false};
    std::string object_id;
    std::string etag;
    std::uint64_t object_size{0};
    std::string error;
  };
  [[nodiscard]] bool CompleteMultipartUpload(
      std::string_view req_id, const std::string& upload_id,
      const std::vector<std::pair<std::uint32_t, std::string>>& parts,
      CompletedMultipart& out) const;

  /** @brief 终止分段上传，proxy 清理会话（幂等）。 */
  [[nodiscard]] bool AbortMultipartUpload(std::string_view req_id, const std::string& upload_id,
                                          std::string& out_error) const;

  // ===== GET 接口（client → proxy）=====

  /** @brief 查对象布局。 */
  [[nodiscard]] bool StatObject(std::string_view req_id, const std::string& bucket,
                                const std::string& key, std::uint64_t& out_object_size,
                                std::string& out_error) const;

  /** @brief GDS 通路 GET：cuObj RDMA token(CUOBJ_GET) 随 RPC 透传，
   * backend RDMA_WRITE 推数据到 client。 */
  [[nodiscard]] bool GdsGet(std::string_view req_id, const std::string& bucket,
                            const std::string& key, std::uint64_t object_size,
                            const GdsDataSource& gds_source, GetPathResult& res) const;

  /** @brief UCX 通路 GET：描述符随 RPC 透传，backend ucp_put_nbx 推数据到
   * client。 */
  [[nodiscard]] bool UcxGet(std::string_view req_id, const std::string& bucket,
                            const std::string& key, std::uint64_t object_size,
                            const UcxDataSource& ucx_source, GetPathResult& res) const;

 private:
  void ApplyTimeout(brpc::Controller& controller) const {
    controller.set_timeout_ms(static_cast<int>(rpc_timeout_.count()));
  }

  [[nodiscard]] us3_turbo::proxy::Control_Stub* stub() const { return stub_.get(); }

  // RPC 超时(用 opts.rpc_timeout)。
  std::chrono::milliseconds rpc_timeout_{};
  std::unique_ptr<brpc::Channel> channel_;
  std::unique_ptr<us3_turbo::proxy::Control_Stub> stub_;
  std::string init_error_;
};

}  // namespace us3_turbo::client
