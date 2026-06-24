#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace us3_turbo::client {

/**
 * @brief Data flow type. The GDS-only client always uses GPUDirect; the enum
 *        is kept as a single value so the wire string ("gds-cuobject") stays
 *        a typed constant rather than a bare literal (proxy validates it).
 */
enum class DataFlow {
  GPUDirect,
};

/**
 * @brief Request parameters for PutObject operations.
 *
 * extra_headers / checksum_policy / idempotency_key 在 GDS-only 链路无服务端
 * 消费(proxy / backend 不读这些字段),已移除;只保留 GDS PUT 真正用到的字段。
 */
struct PutObjectRequest {
  std::string bucket;
  std::string key;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  /**
   * GDS 通路必须显式设置且 > 0：proxy 的 OpenSession 据此校验，backend
   * 据此 RDMA-READ。未设置时 GDS PUT 会被 proxy 以 "expected_size must be > 0" 拒绝。
   */
  std::optional<std::uint64_t> expected_size;

  PutObjectRequest& set_bucket(std::string v)                    { bucket = std::move(v); return *this; }
  PutObjectRequest& set_key(std::string v)                       { key = std::move(v); return *this; }
  PutObjectRequest& set_timeout(std::chrono::milliseconds v)     { timeout = v; return *this; }
  PutObjectRequest& set_expected_size(std::uint64_t v)           { expected_size = v; return *this; }
};


/**
 * @brief Read-only data buffer supplied to upload operations.
 *
 * GDS PUT 链路要求 device buffer;data 指向 cudaMalloc 的显存,调用返回前
 * 必须保持有效。
 */
struct ConstBufferView {
  const void* data{nullptr};
  std::size_t size{0};
};


/**
 * @brief Transfer result returned by successful upload operations.
 *
 * 只保留 GDS PUT 链路真正有值的字段：backend 的 GdsChunkResponse 回填
 * etag + bytes_received + crc32c,其余原字段(server 端从不填写)已移除。
 */
struct TransferOutcome {
  /** Total bytes transferred (== buffer size for single-object PUT). */
  std::size_t bytes_transferred{0};
  /** Service-side request identifier, useful for log correlation. */
  std::string request_id;
  /** Transfer session identifier (control plane). */
  std::string session_id;
  /** ETag assigned by the backend on PUT. */
  std::string etag;
  /**
   * Backend 端对实际 RDMA-READ 到的字节计算的 CRC32C。0 表示未计算。
   * verify_crc32c 开启时,client 据此与本地计算值比对。
   */
  std::uint32_t crc32c{0};
};


/** @brief Returns a stable string identifier for the data flow. */
[[nodiscard]] inline std::string_view ToString(DataFlow flow) {
  switch (flow) {
    case DataFlow::GPUDirect:
      return "gds-cuobject";
  }
  return "unknown";
}

}  // namespace us3_turbo::client
