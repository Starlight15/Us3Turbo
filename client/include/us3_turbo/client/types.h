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

/**
 * @brief Data flow types supported by the client.
 *
 * NONE: No data flow selected (default, will be negotiated).
 * GPUDirect: RDMA via cuObjServer (CUDA device memory, GDS).
 * CPUDirect: UCX RDMA (host-pinned memory, native RDMA).
 */
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
 * @brief Object identity + request context packed together for upload/multipart chains.
 */
struct ObjectDescriptor {
  std::string bucket;
  std::string key;
  DataFlow    data_flow{DataFlow::NONE};
  std::string checksum_policy{"none"};

  std::optional<std::uint64_t> offset;
  std::optional<std::uint64_t> length;

  std::optional<std::size_t> expected_total_size;
  std::string idempotency_key;
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
   * 单对象 PUT 的预期字节数。
   *
   * GDS 通路（DataFlow::GPUDirect）必须显式设置且 > 0：proxy 的 OpenSession
   * 据此校验，backend 据此 RDMA-READ。未设置时 GDS PUT 会被 proxy 以
   * "expected_size must be > 0" 拒绝。
   *
   * 分片上传（MultipartUpload::UploadPart）不读此字段：单 part 的 session
   * length 为空（expected_size=0），gateway 跳过整对象 Reserve。
   *
   * HTTP / RDMA 通路当前不消费此字段，留空即向后兼容。
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
 * @brief Request parameters for GetObject operations.
 */
struct GetObjectRequest {
  std::string bucket;
  std::string key;
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;
  std::string checksum_policy{"none"};
  ProgressCallback progress_callback;

  GetObjectRequest& set_bucket(std::string v)                    { bucket = std::move(v); return *this; }
  GetObjectRequest& set_key(std::string v)                       { key = std::move(v); return *this; }
  GetObjectRequest& set_offset(std::uint64_t v)                  { offset = v; return *this; }
  GetObjectRequest& set_length(std::uint64_t v)                  { length = v; return *this; }
  GetObjectRequest& set_timeout(std::chrono::milliseconds v)     { timeout = v; return *this; }
  GetObjectRequest& set_checksum_policy(std::string v)           { checksum_policy = std::move(v); return *this; }
  GetObjectRequest& set_progress_callback(ProgressCallback v)    { progress_callback = std::move(v); return *this; }
};

/**
 * @brief Request parameters for HeadObject operations.
 */
struct HeadObjectRequest {
  std::string bucket;
  std::string key;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;

  HeadObjectRequest& set_bucket(std::string v)                   { bucket = std::move(v); return *this; }
  HeadObjectRequest& set_key(std::string v)                      { key = std::move(v); return *this; }
  HeadObjectRequest& set_timeout(std::chrono::milliseconds v)    { timeout = v; return *this; }
};

/**
 * @brief Mutable data buffer supplied to download operations.
 */
struct MutableBufferView {
  void* data{nullptr};
  std::size_t size{0};
  BufferType type{BufferType::kHostRegular};
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
 * @brief Object metadata returned by head requests.
 */
struct ObjectMetadata {
  std::size_t content_length{0};
  std::string etag;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
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
  /**
   * Server-computed CRC32C：
   *   HTTP PUT / UploadPart  : 来自响应头 x-amz-checksum-crc32c
   *   HTTP GET               : 来自响应头（P1.3 后），client 端边读边算并比对
   *   RDMA PUT/PUT_PART/GET  : 来自 CommitObject/CommitPart/CommitGet 响应字段
   *   GDS PUT/GET           : 来自 GdsChunkResponse.crc32c
   *
   * GET 路径下若两端 CRC 不一致，TransferPath 直接返回 kInvalidArgument
   * 错误（不会把结果交给调用方）；PUT 路径下 server 校验失败也是同样语义。
   */
  std::optional<std::uint32_t> server_crc32c;
};

/**
 * @brief Result returned by a successful StartUpload call.
 *
 * Shared by the public Client::StartUpload API and the internal
 * MetadataClient / UploadCoordinator layers so all layers use one
 * canonical type instead of parallel structs.
 */
struct StartUploadResult {
  /** Opaque upload identifier issued by the gateway. */
  std::string upload_id;
  /** Maximum bytes accepted per part on this upload (gateway-enforced). */
  std::size_t max_part_size{0};
};

/** @brief Returns a stable string identifier for the data flow. */
[[nodiscard]] std::string_view ToString(DataFlow flow);
/** @brief Returns a stable string identifier for the buffer type. */
[[nodiscard]] std::string_view ToString(BufferType type);
/** @brief Returns a stable string identifier for the operation type. */
[[nodiscard]] std::string_view ToString(OperationType operation);

}  // namespace us3_turbo::client
