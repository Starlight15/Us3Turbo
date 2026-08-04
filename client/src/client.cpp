#include "us3_turbo/client/client.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "client/src/common/crc32c.h"
#include "client/src/common/request.h"
#include "client/src/common/trace.h"
#include "client/src/memory_manager/gds_memory_manager.h"
#include "client/src/memory_manager/rdma_memory_manager.h"
#include "client/src/rpc/proxy_rpc.h"
#include "client/src/transport/gds_get_channel.h"
#include "client/src/transport/gds_put_channel.h"
#include "client/src/transport/rdma_get_channel.h"
#include "client/src/transport/rdma_put_channel.h"
#include "us3_turbo/common/logger.h"

#include <cuda_runtime.h>

namespace us3_turbo::client {

namespace {

spdlog::level::level_enum ParseLogLevel(std::string_view s) {
  if (s == "debug") return spdlog::level::debug;
  if (s == "warn") return spdlog::level::warn;
  if (s == "error") return spdlog::level::err;
  return spdlog::level::info;
}

/*
 * 对 part 数据算 CRC32C，与 proxy 返回的 crc32c 比对。
 * is_device=true 时先 D2H 再计算。
 */
[[nodiscard]] bool VerifyPartCrc32c(std::string_view req_id, ConstBufferView buffer,
                                    std::uint32_t remote_crc32c, bool is_device,
                                    const std::string& tag) {
  std::uint32_t local = 0;
  if (is_device) {
    std::vector<std::byte> host(buffer.size);
    cudaError_t e = cudaMemcpy(host.data(), buffer.data, buffer.size, cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) {
      LOG_ERROR(req_id, "{} verify D2H failed: {}", tag, cudaGetErrorString(e));
      return false;
    }
    local = Crc32c(std::span<const std::byte>(host.data(), host.size()));
  } else {
    local =
        Crc32c(std::span<const std::byte>(static_cast<const std::byte*>(buffer.data), buffer.size));
  }
  if (local == remote_crc32c) {
    LOG_INFO(req_id, "{} crc32c MATCH local={:08x} remote={:08x}", tag, local, remote_crc32c);
    return true;
  }
  LOG_ERROR(req_id, "{} crc32c MISMATCH local={:08x} remote={:08x}", tag, local, remote_crc32c);
  return false;
}

/*
 * 判断指针是否位于 device 显存。失败按 host 处理。
 */
[[nodiscard]] bool IsDevicePointer(const void* ptr) {
  cudaPointerAttributes attr{};
  cudaError_t e = cudaPointerGetAttributes(&attr, ptr);
  return e == cudaSuccess && attr.type == cudaMemoryTypeDevice;
}

}  // namespace

Client::Client(ClientOptions options) : opts_(std::move(options)) {}
Client::~Client() = default;

bool Client::Initialize() {
  if (initialized_) return true;

  us3_turbo::common::Logger::Init(ParseLogLevel(opts_.log_level));

  proxy_ = std::make_unique<ProxyRpc>(opts_.endpoint, opts_.rpc_timeout);
  if (!proxy_->ok()) {
    LOG_SYS_ERROR("proxy channel({}) init failed: {}", opts_.endpoint, proxy_->init_error());
    proxy_.reset();
    return false;
  }

  GdsMemoryManager* gds_mgr = nullptr;
  if (GdsMemoryManager::Instance(gds_mgr)) {
    gds_channel_ = std::make_unique<GdsPutChannel>(opts_, *proxy_, gds_mgr);
    gds_get_channel_ = std::make_unique<GdsGetChannel>(opts_, *proxy_, gds_mgr);
  } else {
    LOG_SYS_WARN("GDS manager unavailable, path=kGds will fail");
    gds_channel_.reset();
    gds_get_channel_.reset();
  }

  RdmaMemoryManager* rdma_mgr = nullptr;
  if (RdmaMemoryManager::Instance(rdma_mgr, opts_.rdma_bind_ip)) {
    rdma_channel_ = std::make_unique<RdmaPutChannel>(opts_, *proxy_, rdma_mgr);
    rdma_get_channel_ = std::make_unique<RdmaGetChannel>(opts_, *proxy_, rdma_mgr);
  } else {
    LOG_SYS_WARN("RDMA manager unavailable, path=kRdma will fail");
    rdma_channel_.reset();
    rdma_get_channel_.reset();
  }

  initialized_ = true;
  return true;
}

void Client::Shutdown() {
  rdma_get_channel_.reset();
  rdma_channel_.reset();
  gds_get_channel_.reset();
  gds_channel_.reset();
  proxy_.reset();
  initialized_ = false;
}

bool Client::initialized() const { return initialized_; }

/*
 * GDS 单步 PUT：校验 → 大小检查 → retry-once → 回填 gds_result。
 */
bool Client::PutObjectGds(const ClientProxyPutRequest& req, ConstBufferView buffer,
                           ClientProxyPutResponse& resp) const {
  if (!initialized_) {
    LOG_ERROR(req.req_id, "Client is not initialized");
    return false;
  }
  if (gds_channel_ == nullptr) {
    LOG_ERROR(req.req_id, "GDS channel not initialized");
    return false;
  }

  const auto max_put = opts_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    LOG_WARN(req.req_id,
             "bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
             "use multipart upload",
             req.bucket, req.key, buffer.size, max_put);
    return false;
  }

  PutPathResult res;
  if (!gds_channel_->PutOnce(req, buffer, res)) {
    std::this_thread::sleep_for(opts_.retry_backoff);
    gds_channel_->PutOnce(req, buffer, res);
  }
  resp.gds_result = res;
  return res.ok;
}

/*
 * RDMA 单步 PUT：校验 → 大小检查 → retry-once → 回填 rdma_result。
 */
bool Client::PutObjectRdma(const ClientProxyPutRequest& req, ConstBufferView buffer,
                            ClientProxyPutResponse& resp) const {
  if (!initialized_) {
    LOG_ERROR(req.req_id, "Client is not initialized");
    return false;
  }
  if (rdma_channel_ == nullptr) {
    LOG_ERROR(req.req_id, "RDMA channel not initialized");
    return false;
  }

  const auto max_put = opts_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    LOG_WARN(req.req_id,
             "bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
             "use multipart upload",
             req.bucket, req.key, buffer.size, max_put);
    return false;
  }

  PutPathResult res;
  if (!rdma_channel_->PutOnce(req, buffer, res)) {
    std::this_thread::sleep_for(opts_.retry_backoff);
    rdma_channel_->PutOnce(req, buffer, res);
  }
  resp.rdma_result = res;
  return res.ok;
}

// ---- 分段上传 ----

GdsMemoryManager* Client::GdsManager() const {
  GdsMemoryManager* mgr = nullptr;
  return GdsMemoryManager::Instance(mgr) ? mgr : nullptr;
}

RdmaMemoryManager* Client::RdmaManager() const {
  RdmaMemoryManager* mgr = nullptr;
  return RdmaMemoryManager::Instance(mgr, opts_.rdma_bind_ip) ? mgr : nullptr;
}

bool Client::CreateMultipartUpload(const std::string& bucket, const std::string& key,
                                   PutDataPath path, std::string& out_upload_id,
                                   std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  const std::string req_id = detail::MakeReqId();
  ::us3_turbo::proxy::PutDataPath proto_path;
  switch (path) {
    case PutDataPath::kGds:  proto_path = ::us3_turbo::proxy::PATH_GDS;  break;
    case PutDataPath::kRdma: proto_path = ::us3_turbo::proxy::PATH_RDMA; break;
    default:
      out_error = "invalid PutDataPath (kNone)";
      return false;
  }
  return proxy_->CreateMultipartUpload(req_id, bucket, key, proto_path, out_upload_id, out_error);
}

/*
 * GDS 分段上传单个 part：注册 token → proxy.UploadPartGds → 可选 CRC。
 */
bool Client::UploadPartGds(const std::string& upload_id, std::uint32_t part_number,
                           ConstBufferView buffer, std::string& out_etag,
                           std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  auto* mgr = GdsManager();
  if (mgr == nullptr) {
    out_error = "GDS manager unavailable";
    return false;
  }

  if (buffer.size > opts_.multipart_part_size) {
    out_error = "part size " + std::to_string(buffer.size) + " exceeds multipart_part_size (" +
                std::to_string(opts_.multipart_part_size) + ")";
    return false;
  }

  const std::string req_id = detail::MakeReqId();

  const bool trace = opts_.latency_trace;
  auto t0 = trace ? detail::clk::now() : detail::clk::time_point{};

  GdsMemoryManager::Token token;
  if (!mgr->AcquireToken(buffer.data, buffer.size, token)) {
    out_error = "failed to acquire GDS token";
    return false;
  }
  const std::string rdma_token(token.str());
  auto t_acquire = trace ? detail::clk::now() : detail::clk::time_point{};

  PutPathResult res;
  const bool rpc_ok =
      proxy_->UploadPartGds(req_id, upload_id, part_number, buffer.size, rdma_token, res);
  auto t_rpc = trace ? detail::clk::now() : detail::clk::time_point{};

  if (!rpc_ok || !res.ok) {
    out_error = res.error_message;
    if (out_error.empty()) out_error = "UploadPartGds rpc failed";
    return false;
  }

  if (opts_.verify_crc32c && res.crc32c != 0) {
    VerifyPartCrc32c(req_id, buffer, res.crc32c, IsDevicePointer(buffer.data), "UploadPartGds");
  }

  if (trace) {
    const detail::LatencyStage stages[] = {{"start", t0}, {"acquire", t_acquire}, {"rpc", t_rpc}};
    detail::TraceLatency(req_id, "UploadPartGds", stages, buffer.size);
  }

  out_etag = res.etag;
  LOG_INFO(req_id, "upload={} part={} size={} etag={}", upload_id, part_number, buffer.size,
           out_etag);
  return true;
}

/*
 * RDMA 分段上传单个 part：注册 MR → proxy.UploadPartRdma → 可选 CRC。
 */
bool Client::UploadPartRdma(const std::string& upload_id, std::uint32_t part_number,
                            ConstBufferView buffer, std::string& out_etag,
                            std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  auto* mgr = RdmaManager();
  if (mgr == nullptr) {
    out_error = "RDMA manager unavailable";
    return false;
  }

  if (buffer.size > opts_.multipart_part_size) {
    out_error = "part size " + std::to_string(buffer.size) + " exceeds multipart_part_size (" +
                std::to_string(opts_.multipart_part_size) + ")";
    return false;
  }

  const std::string req_id = detail::MakeReqId();

  const bool trace = opts_.latency_trace;
  auto t0 = trace ? detail::clk::now() : detail::clk::time_point{};

  RdmaMemoryManager::Descriptor desc;
  if (!mgr->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    out_error = "failed to acquire RDMA descriptor";
    return false;
  }
  auto t_acquire = trace ? detail::clk::now() : detail::clk::time_point{};

  PutPathResult res;
  const bool rpc_ok =
      proxy_->UploadPartRdma(req_id, upload_id, part_number, buffer.size, desc.token, res);
  auto t_rpc = trace ? detail::clk::now() : detail::clk::time_point{};

  if (!rpc_ok || !res.ok) {
    out_error = res.error_message;
    if (out_error.empty()) out_error = "UploadPartRdma rpc failed";
    return false;
  }

  if (opts_.verify_crc32c && res.crc32c != 0) {
    VerifyPartCrc32c(req_id, buffer, res.crc32c, false, "UploadPartRdma");
  }

  if (trace) {
    const detail::LatencyStage stages[] = {{"start", t0}, {"acquire", t_acquire}, {"rpc", t_rpc}};
    detail::TraceLatency(req_id, "UploadPartRdma", stages, buffer.size);
  }

  out_etag = res.etag;
  LOG_INFO(req_id, "upload={} part={} size={} etag={}", upload_id, part_number, buffer.size,
           out_etag);
  return true;
}

bool Client::CompleteMultipartUpload(const std::string& upload_id,
                                     const std::vector<PartInfo>& parts,
                                     CompletedMultipart& out) const {
  if (!initialized_) {
    out.error = "Client not initialized";
    return false;
  }
  const std::string req_id = detail::MakeReqId();
  std::vector<std::pair<std::uint32_t, std::string>> proto_parts;
  proto_parts.reserve(parts.size());
  for (const auto& p : parts) {
    proto_parts.emplace_back(p.part_number, p.etag);
  }
  ProxyRpc::CompletedMultipart rpc_out;
  if (!proxy_->CompleteMultipartUpload(req_id, upload_id, proto_parts, rpc_out)) {
    out = rpc_out;
    return false;
  }
  out = std::move(rpc_out);
  return out.ok;
}

bool Client::AbortMultipartUpload(const std::string& upload_id, std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  const std::string req_id = detail::MakeReqId();
  return proxy_->AbortMultipartUpload(req_id, upload_id, out_error);
}

// ---- GET ----

bool Client::StatObject(const std::string& bucket, const std::string& key,
                        std::uint64_t& out_object_size, std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  if (gds_get_channel_ == nullptr) {
    out_error = "GDS get channel not initialized";
    return false;
  }
  return gds_get_channel_->StatObject(bucket, key, out_object_size, out_error);
}

bool Client::GetObjectGds(const std::string& bucket, const std::string& key,
                          MutableBufferView buffer, GetPathResult& res) const {
  if (!initialized_) {
    LOG_SYS_ERROR("Client not initialized");
    return false;
  }
  if (gds_get_channel_ == nullptr) {
    LOG_SYS_ERROR("GDS get channel not initialized");
    return false;
  }
  return gds_get_channel_->GetOnce(bucket, key, buffer, res);
}

bool Client::GetObjectRdma(const std::string& bucket, const std::string& key,
                           MutableBufferView buffer, GetPathResult& res) const {
  if (!initialized_) {
    LOG_SYS_ERROR("Client not initialized");
    return false;
  }
  if (rdma_get_channel_ == nullptr) {
    LOG_SYS_ERROR("RDMA get channel not initialized");
    return false;
  }
  return rdma_get_channel_->GetOnce(bucket, key, buffer, res);
}

}  // namespace us3_turbo::client
