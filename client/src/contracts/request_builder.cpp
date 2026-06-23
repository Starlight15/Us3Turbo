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
[[nodiscard]] std::chrono::milliseconds MakeContext(const ClientOptions& options,
                                                    std::chrono::milliseconds timeout) {
  return (timeout.count() <= 0) ? options.request_timeout : timeout;
}

}  // namespace

OpenSessionRequest MakeOpenSessionRequest(const ClientOptions& options,
                                          const PutObjectRequest& request) {
  return OpenSessionRequest{
      .timeout = MakeContext(options, request.timeout),
      .bucket = request.bucket,
      .key = request.key,
      .length = request.expected_size,
      .request_id = MakeId(kRequestIdPrefix),
      .session_id = MakeId(kSessionIdPrefix),
  };
}

SessionMeta ImportSession(
    const us3_turbo::proxy::OpenSessionResponse& response) {
  SessionMeta session;
  session.request_id = response.request_id();
  session.session_id = response.session_id();
  session.ticket = response.ticket();
  return session;
}

GdsChunkRequest MakeGdsChunkRequest(const OpenSessionRequest& open,
                                    const SessionMeta& session,
                                    ConstBufferView buffer,
                                    std::string_view rdma_token) {
  return GdsChunkRequest{
      .timeout = open.timeout,                 // 复用 OpenSession 的超时
      .bucket = open.bucket,                   // 与 OpenSession 同一对象
      .key = open.key,
      .request_id = session.request_id,        // 服务端回填的 request_id
      .session_id = session.session_id,
      .transfer_ticket = session.ticket,
      .rdma_token = std::string(rdma_token),
      .chunk_size = buffer.size,
  };
}

TransferOutcome MakeTransferOutcome(const SessionMeta& session,
                                    const us3_turbo::proxy::GdsChunkResponse& response,
                                    ConstBufferView buffer) {
  TransferOutcome outcome;
  outcome.bytes_transferred = buffer.size;
  outcome.request_id        = session.request_id;
  outcome.session_id        = session.session_id;
  outcome.etag              = response.etag();
  return outcome;
}

}  // namespace us3_turbo::client
