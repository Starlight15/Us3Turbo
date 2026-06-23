#include "us3_turbo/client/client.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "client/src/common/errors.h"
#include "client/src/common/rpc_context.h"
#include "client/src/contracts/request_builder.h"
#include "client/src/contracts/requests.h"
#include "client/src/control/meta_rpc.h"
#include "client/src/data/chunk_rpc.h"
#include "client/src/gds/retry_policy.h"
#include "client/src/gds_transport/gds_memory_registry.h"

namespace us3_turbo::client {
namespace {

// GDS 入口校验:buffer.type==kCudaDevice。
[[nodiscard]] Result<bool> PreflightGds(BufferType buffer_type) {
  if (buffer_type != BufferType::kCudaDevice) {
    return Result<bool>::Failure(MakeUnsupportedPath(
        DataFlow::GPUDirect,
        "GDS (requires kCudaDevice GPU buffer) path requires buffer.type=kCudaDevice"));
  }
  return Result<bool>::Success(true);
}

// 一次 PUT 的核心动作:OpenSession → AcquireToken → GdsChunk(GdsPut)。
// 失败时 best-effort AbortSession。所有请求/结果结构由 request_builder 工厂
// 从 PutObjectRequest + buffer 单源装配,不再手写中间字段。
[[nodiscard]] Result<TransferOutcome> PutOnce(const ClientOptions& options,
                                              MetaRpc& meta,
                                              const ChunkRpc& chunk,
                                              const PutObjectRequest& request,
                                              ConstBufferView buffer) {
  auto open_request = MakeOpenSessionRequest(options, request, buffer);
  auto open_response = meta.OpenSession(open_request);
  if (!open_response.success()) {
    return Result<TransferOutcome>::Failure(open_response.error());
  }
  auto session = ImportSession(open_response.value());

  // 取整段 buffer 的 RDMA token。token 析构(RPC 响应返回后)自动释放。
  GdsMemoryRegistry registry;
  auto token = registry.AcquireToken(buffer.data, buffer.size, 0, OperationType::kPut);
  if (!token.success()) {
    (void)meta.AbortSession(session.meta.session_id, open_request.context);
    return Result<TransferOutcome>::Failure(token.error());
  }

  auto chunk_req = MakeGdsChunkRequest(open_request, session, request, buffer,
                                       token.value().str());
  auto response = chunk.Put(chunk_req);
  if (!response.success()) {
    (void)meta.AbortSession(session.meta.session_id, open_request.context);
    return Result<TransferOutcome>::Failure(response.error());
  }

  if (request.progress_callback) {
    request.progress_callback({.bytes_completed = buffer.size,
                               .bytes_total = buffer.size,
                               .data_flow = DataFlow::GPUDirect});
  }
  return Result<TransferOutcome>::Success(
      MakeTransferOutcome(session, response.value(), buffer));
}

}  // namespace

// ============================================================
//  构造 / 析构
//
//  内部状态扁平化持有(无 Impl 中间层):MetaRpc / ChunkRpc 各自接管自己的
//  channel,这里只持两个 RPC 对象 + 配置。析构带外在 .cpp 定义,
//  unique_ptr 成员的完整类型(MetaRpc/ChunkRpc)在此可见。
// ============================================================

Client::Client(ClientOptions options) : options_(std::move(options)) {}

Client::~Client() = default;

// ============================================================
//  生命周期
// ============================================================

Result<bool> Client::Initialize() {
  if (initialized_) {
    return Result<bool>::Success(true);
  }

  // 参数校验与 channel 构建都在 MetaRpc / ChunkRpc 内部完成:endpoint 为空或
  // Init 失败时 ok() 返回 false、init_error() 存原因。这里只查结果。
  // gds_data_endpoint 必填(不再回退到控制面 endpoint)。
  meta_ = std::make_unique<MetaRpc>(options_.endpoint, options_.default_timeout);
  if (!meta_->ok()) {
    std::string err = meta_->init_error();
    meta_.reset();
    return Result<bool>::Failure(MakeError(ErrorCode::kRpcError, err, /*retryable=*/true));
  }

  chunk_ = std::make_unique<ChunkRpc>(options_.gds_data_endpoint, options_.default_timeout);
  if (!chunk_->ok()) {
    std::string err = chunk_->init_error();
    chunk_.reset();
    meta_.reset();
    return Result<bool>::Failure(MakeError(ErrorCode::kRpcError, err, /*retryable=*/true));
  }

  // GDS 通路一次性探测 libcuobjclient:与数据面 lazy 加载同源,失败直接报
  // 带原因的 kTransportError,而非等到首次 PutObject 才崩在半路。
  if (options_.data_flow == DataFlow::GPUDirect) {
    if (auto r = GdsMemoryRegistry::ProbeLibrary(); !r.success()) {
      chunk_.reset();
      meta_.reset();
      return Result<bool>::Failure(r.error());
    }
  }

  initialized_ = true;
  return Result<bool>::Success(true);
}

void Client::Shutdown() {
  // channel 在 RPC 对象内部,随其一起析构;顺序无关。
  chunk_.reset();
  meta_.reset();
  initialized_ = false;
}

bool Client::initialized() const { return initialized_; }

// ============================================================
//  PutObject — GDS 单通路直接实现
// ============================================================

Result<TransferOutcome> Client::PutObject(const PutObjectRequest& request,
                                          ConstBufferView buffer) const {
  if (!initialized_) {
    return Result<TransferOutcome>::Failure(MakeNotInitialized("Client"));
  }

  auto pre = PreflightGds(buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  const auto max_put = options_.gds.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "GDS PUT body " + std::to_string(buffer.size) +
            " exceeds gds.put_single_max_bytes " + std::to_string(max_put) +
            "; use multipart upload",
        /*retryable=*/false));
  }

  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    if (std::chrono::steady_clock::now() >= deadline) {
      return Result<TransferOutcome>::Failure(MakeError(
          ErrorCode::kTimeout, "PutObject retry deadline exceeded", true));
    }
    return PutOnce(options_, *meta_, *chunk_, request, buffer);
  });
}

// ============================================================
//  GPU buffer 注册 / 反注册
// ============================================================

Result<bool> Client::RegisterDeviceBuffer(void* ptr, std::size_t size) {
  if (!initialized_) {
    return Result<bool>::Failure(MakeNotInitialized("Client"));
  }
  return GdsMemoryRegistry{}.RegisterBuffer(ptr, size);
}

Result<bool> Client::UnregisterDeviceBuffer(void* ptr) {
  if (!initialized_) {
    return Result<bool>::Failure(MakeNotInitialized("Client"));
  }
  return GdsMemoryRegistry{}.UnregisterBuffer(ptr);
}

}  // namespace us3_turbo::client
