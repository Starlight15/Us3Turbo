#include "client/src/contracts/request_builder.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <utility>

namespace us3_turbo::client {

namespace {

constexpr char kRequestIdPrefix[] = "req-";
constexpr char kSessionIdPrefix[] = "ses-";
constexpr char kDefaultGatewayId[] = "gateway-local";

[[nodiscard]] std::string MakeId(std::string_view prefix) {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::random_device{}()) ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())};
  const std::uint64_t value = rng();
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", value);
  return std::string(prefix) + buf;
}

// 优先级:request.timeout > request_timeout(单对象 PUT 无 per-op 拆分)。
[[nodiscard]] RpcCallMetadata MakeContext(const ClientOptions& options,
                                          std::chrono::milliseconds timeout) {
  const auto effective =
      (timeout.count() <= 0) ? options.request_timeout : timeout;
  return RpcCallMetadata{.client_id = options.client_id,
                         .bearer_token = options.bearer_token,
                         .default_headers = options.default_headers,
                         .timeout = effective};
}

}  // namespace

OpenSessionRequest MakeOpenSessionRequest(const ClientOptions& options,
                                          const PutObjectRequest& request,
                                          ConstBufferView buffer) {
  return OpenSessionRequest{
      .context = MakeContext(options, request.timeout),
      .operation = OperationType::kPut,
      .bucket = request.bucket,
      .key = request.key,
      .data_flow = DataFlow::GPUDirect,
      .buffer_type = buffer.type,
      .offset = 0,
      .length = request.expected_size,
      .request_id = MakeId(kRequestIdPrefix),
      .session_id = MakeId(kSessionIdPrefix),
      .idempotency_key = request.idempotency_key,
      .is_multipart_part = false,
  };
}

TransferSession ImportSession(
    const us3_turbo::proxy::OpenSessionResponse& response) {
  TransferSession session;
  session.meta.request_id = response.request_id();
  session.meta.session_id = response.session_id();
  session.meta.ticket = response.ticket();
  session.meta.expire_at = response.expire_at();
  session.meta.gateway_id = response.gateway_id().empty()
                                ? kDefaultGatewayId
                                : response.gateway_id();
  return session;
}

GdsChunkRequest MakeGdsChunkRequest(const OpenSessionRequest& open,
                                    const TransferSession& session,
                                    const PutObjectRequest& request,
                                    ConstBufferView buffer,
                                    std::string_view rdma_token) {
  return GdsChunkRequest{
      .context = open.context,                 // 复用 OpenSession 的 RPC 上下文
      .operation = open.operation,             // kPut
      .bucket = open.bucket,                   // 与 OpenSession 同一对象
      .key = open.key,
      .data_flow = open.data_flow,             // GPUDirect
      .buffer_type = open.buffer_type,         // kCudaDevice
      .checksum_policy = request.checksum_policy,
      .extra_headers = request.extra_headers,
      .request_id = session.meta.request_id,   // 服务端回填的 request_id
      .session_id = session.meta.session_id,
      .transfer_ticket = session.meta.ticket,
      .rdma_token = std::string(rdma_token),
      .chunk_offset = 0,                       // 单对象 PUT:整段一次传
      .chunk_size = buffer.size,
  };
}

TransferOutcome MakeTransferOutcome(const TransferSession& session,
                                    const us3_turbo::proxy::GdsChunkResponse& response,
                                    ConstBufferView buffer) {
  TransferOutcome outcome;
  outcome.selected_flow     = DataFlow::GPUDirect;
  outcome.request_id        = session.meta.request_id;
  outcome.session_id        = session.meta.session_id;
  outcome.gateway_id        = response.selected_gateway().empty()
                                  ? session.meta.gateway_id
                                  : response.selected_gateway();
  outcome.transfer_status   = response.transfer_status().empty()
                                  ? std::string{"completed"}
                                  : response.transfer_status();
  outcome.rdma_reply        = response.rdma_reply();
  outcome.etag              = response.etag();
  outcome.version           = response.version();
  outcome.bytes_transferred = buffer.size;
  if (response.crc32c() != 0) {
    outcome.server_crc32c = response.crc32c();
  }
  return outcome;
}

}  // namespace us3_turbo::client
