#include "us3_turbo/client/client.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cuda_runtime.h>
#include <spdlog/spdlog.h>

#include "client/src/common/request.h"
#include "client/src/common/crc32c.h"
#include "client/src/common/trace.h"
#include "client/src/memory_manager/gds_memory_manager.h"
#include "client/src/rpc/proxy_rpc.h"
#include "client/src/memory_manager/ucx_memory_manager.h"
#include "client/src/transport/gds_put_channel.h"
#include "client/src/transport/put_channel.h"
#include "client/src/transport/ucx_put_channel.h"

namespace us3_turbo::client {

namespace {

// retry-once 退避。
constexpr auto kRetryBackoff = std::chrono::milliseconds(100);

// client 侧对 part 数据算 CRC32C（GDS 需 D2H；UCX 直算），与 proxy 返回的
// PutPathResult.crc32c 比对做端到端校验（options.verify_crc32c 开启时）。
[[nodiscard]] bool VerifyPartCrc32c(std::string_view request_id,
                                    ConstBufferView buffer,
                                    std::uint32_t remote_crc32c,
                                    bool is_device,
                                    const std::string& tag) {
  std::uint32_t local = 0;
  if (is_device) {
    std::vector<std::byte> host(buffer.size);
    if (cudaError_t e = cudaMemcpy(host.data(), buffer.data, buffer.size,
                                   cudaMemcpyDeviceToHost);
        e != cudaSuccess) {
      spdlog::error("{} (req={}): verify D2H failed: {}", request_id, tag,
                    cudaGetErrorString(e));
      return false;
    }
    local = Crc32c(std::span<const std::byte>(host.data(), host.size()));
  } else {
    local = Crc32c(std::span<const std::byte>(
        static_cast<const std::byte*>(buffer.data), buffer.size));
  }
  if (local == remote_crc32c) {
    spdlog::info("{} (req={}): crc32c MATCH local={:08x} remote={:08x}",
                 request_id, tag, local, remote_crc32c);
    return true;
  }
  spdlog::error("{} (req={}): crc32c MISMATCH local={:08x} remote={:08x}",
                request_id, tag, local, remote_crc32c);
  return false;
}

[[nodiscard]] bool IsDevicePointer(const void* ptr) {
  cudaPointerAttributes attr{};
  if (cudaError_t e = cudaPointerGetAttributes(&attr, ptr);
      e != cudaSuccess) {
    return false;  // 当 host 指针处理
  }
  return attr.type == cudaMemoryTypeDevice;
}

}  // namespace

Client::Client(ClientOptions options) : options_(std::move(options)) {}
Client::~Client() = default;

bool Client::Initialize() {
  if (initialized_) return true;

  // 单 brpc channel 指向 proxy,线程安全,可被多 worker 并发调用。
  proxy_ = std::make_unique<ProxyRpc>(options_.endpoint, options_.default_timeout);
  if (!proxy_->ok()) {
    spdlog::error("Initialize: proxy channel({}) init failed: {}",
                  options_.endpoint, proxy_->init_error());
    proxy_.reset();
    return false;
  }

  // manager 不可用则 channel 留空,该 path 落到 SelectChannel 返回 nullptr。
  GdsMemoryManager* gds_mgr = nullptr;
  if (GdsMemoryManager::Instance(gds_mgr)) {
    gds_channel_ = std::make_unique<GdsPutChannel>(options_, *proxy_, gds_mgr);
  } else {
    spdlog::warn("Client::Initialize: GDS manager unavailable, "
                 "path=kGds will fail");
    gds_channel_.reset();
  }

  // UCX 同构,Start 失败不致命。
  UcxMemoryManager* ucx_mgr = nullptr;
  if (UcxMemoryManager::Instance(ucx_mgr)) {
    ucx_channel_ = std::make_unique<UcxPutChannel>(options_, *proxy_, ucx_mgr);
  } else {
    spdlog::warn("Client::Initialize: UCX manager unavailable, "
                 "path=kUcx will fail");
    ucx_channel_.reset();
  }

  initialized_ = true;
  return true;
}

void Client::Shutdown() {
  ucx_channel_.reset();
  gds_channel_.reset();
  proxy_.reset();
  initialized_ = false;
}

bool Client::initialized() const { return initialized_; }

// path 校验:kNone / kAll 拒绝(单 buffer 无法双路)。
bool Client::ValidatePutPath(const ClientProxyPutRequest& req) const {
  if (req.path == PutDataPath::kNone) {
    spdlog::error("PutObject: path not specified (req={})", req.request_id);
    return false;
  }
  if (req.path == PutDataPath::kAll) {
    spdlog::error("PutObject: kAll not supported yet (req={})", req.request_id);
    return false;
  }
  return true;
}

// 路由落点。
PutChannel* Client::SelectChannel(PutDataPath path) const noexcept {
  switch (path) {
    case PutDataPath::kGds: return gds_channel_.get();
    case PutDataPath::kUcx: return ucx_channel_.get();
    default:            return nullptr;  // kNone / kAll(已在校验阶段拒绝)
  }
}

bool Client::PutObject(const ClientProxyPutRequest& request,
                       ConstBufferView buffer,
                       ClientProxyPutResponse& response) const {
  if (!initialized_) {
    spdlog::error("PutObject: Client is not initialized. Call Client::Initialize first. "
                  "(req={})", request.request_id);
    return false;
  }
  if (!ValidatePutPath(request)) {
    return false;
  }

  // 大小上限校验。
  const auto max_put = options_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    spdlog::warn("PutObject: bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
                 "use multipart upload",
                 request.bucket, request.key, buffer.size, max_put);
    return false;
  }

  PutChannel* ch = SelectChannel(request.path);
  if (ch == nullptr) {
    spdlog::error("PutObject: {} channel not initialized (req={})",
                  request.path == PutDataPath::kGds ? "GDS" : "UCX",
                  request.request_id);
    return false;
  }

  PutPathResult result;

  // retry-once:首次失败等 100ms 再试一次,接受最终结果。
  if (!ch->PutOnce(request, buffer, result)) {
    std::this_thread::sleep_for(kRetryBackoff);
    ch->PutOnce(request, buffer, result);
  }

  // 按 path 回填结果到对应字段。
  if (request.path == PutDataPath::kGds) response.gds_result = result;
  else                                   response.ucx_result = result;

  return result.ok;
}

// ===========================================================================
// 分段上传。与单步 PutObject 隔离：不复用 PutChannel，直接调 proxy RPC，
// 每个 part 独立注册 token/descriptor。
// ===========================================================================

GdsMemoryManager* Client::GdsManager() const {
  GdsMemoryManager* mgr = nullptr;
  return GdsMemoryManager::Instance(mgr) ? mgr : nullptr;
}

UcxMemoryManager* Client::UcxManager() const {
  UcxMemoryManager* mgr = nullptr;
  return UcxMemoryManager::Instance(mgr) ? mgr : nullptr;
}

bool Client::CreateMultipartUpload(
    const std::string& bucket,
    const std::string& key,
    PutDataPath path,
    std::string& out_upload_id,
    std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  const std::string request_id = detail::MakeRequestId();
  const ::us3_turbo::proxy::PutDataPath proto_path =
      (path == PutDataPath::kGds) ? ::us3_turbo::proxy::PATH_GDS
                                  : ::us3_turbo::proxy::PATH_UCX;
  return proxy_->CreateMultipartUpload(request_id, bucket, key, proto_path,
                                       out_upload_id, out_error);
}

bool Client::UploadPartGds(
    const std::string& upload_id,
    std::uint32_t part_number,
    ConstBufferView buffer,
    std::string& out_etag,
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

  // 分段 part 上限：16 MiB（与单步对象上限对齐）。超出拒绝。
  constexpr std::uint64_t kMaxPartBytes = 16ULL * 1024 * 1024;
  if (buffer.size > kMaxPartBytes) {
    out_error = "part size " + std::to_string(buffer.size) +
                " exceeds 16MiB per-part limit";
    return false;
  }

  const std::string request_id = detail::MakeRequestId();

  // 为本 part 独立注册 token（offset=0，相对本 part buffer）。
  GdsMemoryManager::Token token;
  if (!mgr->AcquireToken(buffer.data, buffer.size, 0, token)) {
    out_error = "failed to acquire GDS token";
    return false;
  }
  const std::string rdma_token(token.str());

  PutPathResult result;
  const bool rpc_ok = proxy_->UploadPartGds(request_id, upload_id, part_number,
                                            buffer.size, rdma_token, result);
  // Token 析构自动释放（RAII），无需显式 ReleaseToken。

  if (!rpc_ok || !result.ok) {
    out_error = result.ok ? result.error_message : result.error_message;
    if (out_error.empty()) out_error = "UploadPartGds rpc failed";
    return false;
  }

  // 可选 CRC 校验（仅当 server 返回了 crc32c）。
  if (options_.verify_crc32c && result.crc32c != 0) {
    VerifyPartCrc32c(request_id, buffer, result.crc32c,
                     IsDevicePointer(buffer.data), "UploadPartGds");
  }

  out_etag = result.etag;
  spdlog::info("UploadPartGds (req={}): upload={} part={} size={} etag={}",
               request_id, upload_id, part_number, buffer.size, out_etag);
  return true;
}

bool Client::UploadPartUcx(
    const std::string& upload_id,
    std::uint32_t part_number,
    ConstBufferView buffer,
    std::string& out_etag,
    std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  auto* mgr = UcxManager();
  if (mgr == nullptr) {
    out_error = "UCX manager unavailable";
    return false;
  }

  // 分段 part 上限：16 MiB（与单步对象上限对齐）。超出拒绝。
  constexpr std::uint64_t kMaxPartBytes = 16ULL * 1024 * 1024;
  if (buffer.size > kMaxPartBytes) {
    out_error = "part size " + std::to_string(buffer.size) +
                " exceeds 16MiB per-part limit";
    return false;
  }

  const std::string request_id = detail::MakeRequestId();

  UcxMemoryManager::Descriptor desc;
  if (!mgr->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    out_error = "failed to acquire UCX descriptor";
    return false;
  }

  PutPathResult result;
  const bool rpc_ok = proxy_->UploadPartUcx(request_id, upload_id, part_number,
                                            buffer.size, desc.remote_addr,
                                            desc.rkey, desc.client_ucx_addr,
                                            result);
  if (!rpc_ok || !result.ok) {
    out_error = result.error_message;
    if (out_error.empty()) out_error = "UploadPartUcx rpc failed";
    return false;
  }

  if (options_.verify_crc32c && result.crc32c != 0) {
    VerifyPartCrc32c(request_id, buffer, result.crc32c, false, "UploadPartUcx");
  }

  out_etag = result.etag;
  spdlog::info("UploadPartUcx (req={}): upload={} part={} size={} etag={}",
               request_id, upload_id, part_number, buffer.size, out_etag);
  return true;
}

bool Client::CompleteMultipartUpload(
    const std::string& upload_id,
    const std::vector<PartInfo>& parts,
    CompletedMultipart& out) const {
  if (!initialized_) {
    out.error = "Client not initialized";
    return false;
  }
  const std::string request_id = detail::MakeRequestId();
  std::vector<std::pair<std::uint32_t, std::string>> proto_parts;
  proto_parts.reserve(parts.size());
  for (const auto& p : parts) {
    proto_parts.emplace_back(p.part_number, p.etag);
  }
  ProxyRpc::CompletedMultipart rpc_out;
  if (!proxy_->CompleteMultipartUpload(request_id, upload_id, proto_parts,
                                       rpc_out)) {
    out = rpc_out;  // 失败时也拷贝 error
    return false;
  }
  out = std::move(rpc_out);
  return out.ok;
}

}  // namespace us3_turbo::client
