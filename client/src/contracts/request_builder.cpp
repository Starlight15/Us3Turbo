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

PutAttempt MakePutAttempt(const ClientOptions& options,
                          const PutObjectRequest& request) {
  return PutAttempt{
      .timeout = MakeContext(options, request.timeout),
      .bucket = request.bucket,
      .key = request.key,
      .length = request.expected_size,
      .request_id = MakeId(kRequestIdPrefix),
      .session_id = MakeId(kSessionIdPrefix),
  };
}

TransferOutcome MakeTransferOutcome(const PutAttempt& attempt,
                                    const GdsPutResult& result,
                                    ConstBufferView buffer) {
  TransferOutcome outcome;
  outcome.bytes_transferred = buffer.size;
  outcome.request_id        = attempt.request_id;
  outcome.session_id        = attempt.session_id;
  outcome.etag              = result.etag;
  return outcome;
}

}  // namespace us3_turbo::client
