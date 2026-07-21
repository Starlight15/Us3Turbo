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
#include "client/src/transport/rdma_put_channel.h"
#include "us3_turbo/common/logger.h"

#include <cuda_runtime.h>

namespace us3_turbo::client {

namespace {

/* log_level 字符串 → spdlog 级别（ClientOptions::log_level）。 */
spdlog::level::level_enum ParseLogLevel(std::string_view s) {
  if (s == "debug") return spdlog::level::debug;
  if (s == "warn") return spdlog::level::warn;
  if (s == "error") return spdlog::level::err;
  return spdlog::level::info;  // "info" / 未知
}

/**
 * @brief client 侧对 part 数据算 CRC32C，与 proxy 返回的 PutPathResult.crc32c
 * 比对做端到端校验（options.verify_crc32c 开启时）。
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

/**
 * @brief 判断指针是否位于 device 显存。失败按 host 指针处理。
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

  // 初始化日志（client 默认仅控制台 sink）。
  us3_turbo::common::Logger::Init(ParseLogLevel(opts_.log_level));

  // 单 brpc channel 指向 proxy,线程安全,可被多 worker 并发调用。
  proxy_ = std::make_unique<ProxyRpc>(opts_.endpoint, opts_.rpc_timeout);
  if (!proxy_->ok()) {
    LOG_SYS_ERROR("proxy channel({}) init failed: {}", opts_.endpoint, proxy_->init_error());
    proxy_.reset();
    return false;
  }

  // manager 不可用则 channel 留空,该 path 落到 SelectChannel 返回 nullptr。
  GdsMemoryManager* gds_mgr = nullptr;
  if (GdsMemoryManager::Instance(gds_mgr)) {
    gds_channel_ = std::make_unique<GdsPutChannel>(opts_, *proxy_, gds_mgr);
    gds_get_channel_ = std::make_unique<GdsGetChannel>(opts_, *proxy_, gds_mgr);
  } else {
    LOG_SYS_WARN("GDS manager unavailable, path=kGds will fail");
    gds_channel_.reset();
    gds_get_channel_.reset();
  }

  // RDMA 同构，Start 失败不致命。
  RdmaMemoryManager* rdma_mgr = nullptr;
  if (RdmaMemoryManager::Instance(rdma_mgr)) {
    rdma_channel_ = std::make_unique<RdmaPutChannel>(opts_, *proxy_, rdma_mgr);
  } else {
    LOG_SYS_WARN("RDMA manager unavailable, path=kRdma will fail");
    rdma_channel_.reset();
  }

  initialized_ = true;
  return true;
}

void Client::Shutdown() {
  rdma_channel_.reset();
  gds_get_channel_.reset();
  gds_channel_.reset();
  proxy_.reset();
  initialized_ = false;
}

bool Client::initialized() const { return initialized_; }

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

  // 大小上限校验。
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

  // 大小上限校验。
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

// ===========================================================================
// 分段上传。与单步 PutObject 隔离：不复用 PutChannel，直接调 proxy RPC，
// 每个 part 独立注册 token/descriptor。
// ===========================================================================

GdsMemoryManager* Client::GdsManager() const {
  GdsMemoryManager* mgr = nullptr;
  return GdsMemoryManager::Instance(mgr) ? mgr : nullptr;
}

RdmaMemoryManager* Client::RdmaManager() const {
  RdmaMemoryManager* mgr = nullptr;
  return RdmaMemoryManager::Instance(mgr) ? mgr : nullptr;
}

bool Client::CreateMultipartUpload(const std::string& bucket, const std::string& key,
                                   PutDataPath path, std::string& out_upload_id,
                                   std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  const std::string req_id = detail::MakeReqId();
  const ::us3_turbo::proxy::PutDataPath proto_path =
      (path == PutDataPath::kGds) ? ::us3_turbo::proxy::PATH_GDS : ::us3_turbo::proxy::PATH_RDMA;
  return proxy_->CreateMultipartUpload(req_id, bucket, key, proto_path, out_upload_id, out_error);
}

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

  // 分段 part 上限：须 ≤ multipart_part_size（默认 4MiB，与 proxy 对齐）。
  // 非 last part 必须恰好等于此值；仅 last part 可小于此值。
  // 违反规则将在 CompleteMultipartUpload 时被 proxy 拒绝。
  if (buffer.size > opts_.multipart_part_size) {
    out_error = "part size " + std::to_string(buffer.size) + " exceeds multipart_part_size (" +
                std::to_string(opts_.multipart_part_size) + ")";
    return false;
  }

  const std::string req_id = detail::MakeReqId();

  // [诊断插桩] 复用现有 latency_trace 开关(bench --trace 已透传到
  // opts_.latency_trace)拆分 acquire(client 侧注册/token)与 rpc(网络+backend
  // 可见耗时)两阶段。验证完毕后可整块删除。
  const bool trace = opts_.latency_trace;
  auto t0 = trace ? detail::clk::now() : detail::clk::time_point{};

  // 为本 part 独立注册 token（offset=0，相对本 part buffer）。
  GdsMemoryManager::Token token;
  if (!mgr->AcquireToken(buffer.data, buffer.size, 0, token)) {
    out_error = "failed to acquire GDS token";
    return false;
  }
  const std::string rdma_token(token.str());
  auto t_acquire = trace ? detail::clk::now() : detail::clk::time_point{};

  PutPathResult res;
  const bool rpc_ok =
      proxy_->UploadPartGds(req_id, upload_id, part_number, buffer.size, rdma_token, res);
  auto t_rpc = trace ? detail::clk::now() : detail::clk::time_point{};
  // Token 析构自动释放（RAII），无需显式 ReleaseToken。

  if (!rpc_ok || !res.ok) {
    out_error = res.ok ? res.error_message : res.error_message;
    if (out_error.empty()) out_error = "UploadPartGds rpc failed";
    return false;
  }

  // 可选 CRC 校验（仅当 server 返回了 crc32c）。
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

  // 分段 part 上限：须 ≤ multipart_part_size（默认 4MiB，与 proxy 对齐）。
  // 非 last part 必须恰好等于此值；仅 last part 可小于此值。
  // 违反规则将在 CompleteMultipartUpload 时被 proxy 拒绝。
  if (buffer.size > opts_.multipart_part_size) {
    out_error = "part size " + std::to_string(buffer.size) + " exceeds multipart_part_size (" +
                std::to_string(opts_.multipart_part_size) + ")";
    return false;
  }

  const std::string req_id = detail::MakeReqId();

  // [诊断插桩] 复用现有 latency_trace 开关拆分 acquire(rkey+token) 与 rpc 两阶段。
  const bool trace = opts_.latency_trace;
  auto t0 = trace ? detail::clk::now() : detail::clk::time_point{};

  // 为本 part 独立注册 MR + 编码 token（offset=0，相对本 part buffer）。
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

  // 可选 CRC 校验（RDMA 为 host buffer，无需 D2H）。
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
    out = rpc_out;  // 失败时也拷贝 error
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

// ===========================================================================
// GET（StatObject / GetObjectGds）
// ===========================================================================

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

}  // namespace us3_turbo::client
