#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "us3_turbo/common/error_code.h"

namespace us3_turbo::client {

/**
 * @brief Error codes returned by the public client API.
 *
 * The enum is an alias of the project-wide canonical enum so the same code
 * values flow between the client SDK and the gateway without translation.
 */
using ErrorCode = ::us3_turbo::common::ErrorCode;

/** Data flow types. GPUDirect is the only supported path; others are rejected by Initialize(). */
enum class DataFlow {
  NONE,
  GPUDirect,
  CPUDirect,
};

/**
 * @brief Memory buffer categories accepted by transfer APIs.
 */
enum class BufferType {
  kHostRegular,
  kHostPinned,
  kCudaDevice,
};

/**
 * @brief Object operations issued through the client.
 */
enum class OperationType {
  kGet,
  kPut,
  kHead,
};


/**
 * @brief Progress snapshot reported during a transfer.
 */
struct TransferProgress {
  std::size_t bytes_completed{0};
  std::size_t bytes_total{0};
  DataFlow data_flow{DataFlow::NONE};
};

/**
 * @brief Callback invoked with incremental transfer progress.
 */
using ProgressCallback = std::function<void(const TransferProgress&)>;

/**
 * @brief Request parameters for PutObject operations.
 */
struct PutObjectRequest {
  std::string bucket;
  std::string key;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;
  std::string checksum_policy{"none"};
  std::string idempotency_key;
  ProgressCallback progress_callback;
  /**
   * GDS 通路必须显式设置且 > 0：proxy 的 OpenSession 据此校验，backend
   * 据此 RDMA-READ。未设置时 GDS PUT 会被 proxy 以 "expected_size must be > 0" 拒绝。
   */
  std::optional<std::uint64_t> expected_size;

  PutObjectRequest& set_bucket(std::string v)                    { bucket = std::move(v); return *this; }
  PutObjectRequest& set_key(std::string v)                       { key = std::move(v); return *this; }
  PutObjectRequest& set_timeout(std::chrono::milliseconds v)     { timeout = v; return *this; }
  PutObjectRequest& set_checksum_policy(std::string v)           { checksum_policy = std::move(v); return *this; }
  PutObjectRequest& set_idempotency_key(std::string v)           { idempotency_key = std::move(v); return *this; }
  PutObjectRequest& set_progress_callback(ProgressCallback v)    { progress_callback = std::move(v); return *this; }
  PutObjectRequest& set_expected_size(std::uint64_t v)           { expected_size = v; return *this; }
};


/**
 * @brief Read-only data buffer supplied to upload operations.
 */
struct ConstBufferView {
  const void* data{nullptr};
  std::size_t size{0};
  BufferType type{BufferType::kHostRegular};
};


/**
 * @brief Transfer result returned by successful upload and download operations.
 */
struct TransferOutcome {
  /** Data flow actually used for the transfer (may differ from request). */
  DataFlow selected_flow{DataFlow::NONE};
  /** Total bytes transferred. */
  std::size_t bytes_transferred{0};
  /** Service-side request identifier, useful for log correlation. */
  std::string request_id;
  /** Transfer session identifier (control plane). */
  std::string session_id;
  /** Server-reported terminal status for this transfer (e.g. "completed"). */
  std::string transfer_status;
  /** Identifier of the gateway that handled the transfer. */
  std::string gateway_id;
  /** Path-specific reply string (RDMA path echoes acknowledgement data here). */
  std::string rdma_reply;
  /** ETag assigned by the backend on PUT/UploadPart, empty on GET. */
  std::string etag;
  /** Object version assigned by the backend, when versioning is enabled. */
  std::string version;
  /** Server-computed CRC32C from GdsChunkResponse.crc32c. */
  std::optional<std::uint32_t> server_crc32c;
};


/** @brief Returns a stable string identifier for the data flow. */
[[nodiscard]] std::string_view ToString(DataFlow flow);
/** @brief Returns a stable string identifier for the buffer type. */
[[nodiscard]] std::string_view ToString(BufferType type);
/** @brief Returns a stable string identifier for the operation type. */
[[nodiscard]] std::string_view ToString(OperationType operation);

}  // namespace us3_turbo::client
