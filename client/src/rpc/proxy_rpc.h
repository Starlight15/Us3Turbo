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

// brpc channel 最大重试次数（含首次，共 max_retry 次）。
constexpr int kBrpcMaxRetry = 2;

/**
 * @brief proxy 控制面 RPC client:GdsPut / RdmaPut 共用一条 brpc channel。
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
    co.max_retry = kBrpcMaxRetry;
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
                            const std::string& rdma_token, PutPathResult& res) const;

  /** @brief RDMA (libibverbs) 通路:token 随 RPC 透传,backend RDMA CM 反向连接后
   * ibv_post_send(RDMA_READ)。 */
  [[nodiscard]] bool RdmaPut(std::string_view req_id, const std::string& bucket,
                             const std::string& key, std::uint64_t object_size,
                             const std::string& rdma_token, PutPathResult& res) const;

  // ===== 分段上传接口（client → proxy）=====

  /** @brief 初始化分段上传，返回 upload_id 与 proxy 生成的 trace_id。 */
  [[nodiscard]] bool CreateMultipartUpload(std::string_view req_id, const std::string& bucket,
                                           const std::string& key,
                                           ::us3_turbo::proxy::PutDataPath path,
                                           std::string& out_upload_id, std::string& out_trace_id,
                                           std::string& out_error) const;

  /** @brief GDS 路径上传单个 part：rdma_token 随 RPC 透传。trace_id 由 proxy
   * 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool UploadPartGds(std::string_view req_id,
                                   const std::string& upload_id,
                                   std::uint32_t part_number, std::uint64_t part_size,
                                   const std::string& rdma_token, PutPathResult& res) const;

  /** @brief RDMA (libibverbs) 路径上传单个 part：rdma_token 随 RPC 透传。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool UploadPartRdma(std::string_view req_id,
                                    const std::string& upload_id,
                                    std::uint32_t part_number, std::uint64_t part_size,
                                    const std::string& rdma_token, PutPathResult& res) const;

  /** @brief 完成分段上传，返回最终 object_id/etag/size。trace_id 由 proxy
   * 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  struct CompletedMultipart {
    bool ok{false};
    std::string object_id;
    std::string etag;
    std::uint64_t object_size{0};
    std::string error;
    std::uint64_t trace_id{0};  // proxy snowflake trace_id(从 RPC 响应取)
  };
  [[nodiscard]] bool CompleteMultipartUpload(
      std::string_view req_id,
      const std::string& upload_id,
      const std::vector<std::pair<std::uint32_t, std::string>>& parts,
      CompletedMultipart& out) const;

  /** @brief 终止分段上传，proxy 清理会话（幂等）。trace_id 由 proxy 每请求
   * 生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool AbortMultipartUpload(std::string_view req_id,
                                          const std::string& upload_id,
                                          std::string& out_error) const;

  // ===== GET 接口（client → proxy）=====

  /** @brief 查对象布局。trace_id 由 proxy 生成并带回。 */
  [[nodiscard]] bool StatObject(std::string_view req_id, const std::string& bucket,
                                const std::string& key, std::uint64_t& out_object_size,
                                std::string& out_trace_id, std::string& out_error) const;

  /** @brief GDS 通路 GET：cuObj RDMA token(CUOBJ_GET) 随 RPC 透传，
   * backend RDMA_WRITE 推数据到 client。trace_id 由 proxy 每请求生成、在响应里
   * 回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool GdsGet(std::string_view req_id,
                            const std::string& bucket,
                            const std::string& key, std::uint64_t object_size,
                            const std::string& rdma_token, GetPathResult& res) const;

  /** @brief RDMA (libibverbs) 通路 GET：token 随 RPC 透传，
   * backend 从 NVMe 读数据后 RDMA WRITE 推数据到 client host buffer。
   * trace_id 由 proxy 每请求生成、在响应里回 client(非入参,对齐 s3proxy)。 */
  [[nodiscard]] bool RdmaGet(std::string_view req_id,
                             const std::string& bucket,
                             const std::string& key, std::uint64_t object_size,
                             const std::string& rdma_token, GetPathResult& res) const;

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
