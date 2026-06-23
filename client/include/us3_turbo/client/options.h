#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <spdlog/logger.h>

#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief GDS-specific client tuning.
 *
 * Reserved for cuObject scheduling / buffer / checksum tuning.
 */
struct GdsClientOptions {
  /**
   * Object length above which GetObject splits into parallel sub-ranges
   * (client-side concurrency on top of gateway-side chunk_plan).
   * 默认 64 MiB：阈值偏高，因 GDS 单 cuObject Get 已经高效。
   */
  std::size_t parallel_get_threshold{64ULL * 1024 * 1024};
  /** Number of parallel sub-ranges issued by split GETs. */
  std::size_t parallel_get_chunks{4};

  /**
   * 单次 GDS PUT/UploadPart 的上限（client 入口拒）。与 gateway 端
   * cuObjServer 的 1 GiB chunk 上限对齐，避免发到 server 才被拒。
   * 0 表示不限。
   */
  std::size_t put_single_max_bytes{1ULL * 1024 * 1024 * 1024};
};

/**
 * @brief HTTP transport tunables.
 *
 * Common timeouts continue to flow through ClientOptions::default_timeout;
 * fields here are HTTP-only behaviours.
 */
struct HttpClientOptions {
  /** Object length above which GetObject splits into parallel sub-ranges. */
  std::size_t parallel_get_threshold{16ULL * 1024 * 1024};
  /** Number of parallel sub-ranges issued by split GETs. */
  std::size_t parallel_get_chunks{8};

  /** Inject x-amz-checksum-crc32c on PUT / UploadPart. */
  bool send_crc32c{false};  // 关闭 CRC32C 以提升性能
  /** Verify server-echoed x-amz-checksum-crc32c against the client value. */
  bool verify_response_crc32c{true};

  /** Maximum total attempts (including the initial one) for retried calls. */
  int max_retry_attempts{3};
  /** Initial exponential backoff delay between retries. */
  std::chrono::milliseconds retry_initial_backoff{std::chrono::milliseconds(100)};
  /** Upper bound for the exponential backoff between retries. */
  std::chrono::milliseconds retry_max_backoff{std::chrono::milliseconds(2000)};

  /**
   * 单次 PutObject 接受的最大 body 字节数。超过此值在 client 端立刻返回
   * kPayloadTooLarge，避免把超大 buffer 推到 TCP 再被 server 413。
   * 默认 5 GiB —— 与 AWS S3 单段 PUT 服务端硬限对齐；如果 gateway 端
   * http_max_put_bytes 更严（默认 1 GiB），实际仍以 server 端为准。
   * 大于此值的对象请改用 StartUpload + UploadPart 分片上传。
   */
  std::size_t put_single_max_bytes{5ULL * 1024ULL * 1024ULL * 1024ULL};
};

/**
 * @brief Native RDMA transport tunables (hardware + connection pool).
 *
 * Independent from GdsClientOptions: the two transports use different
 * libraries (libibverbs vs cuObject) and do not share state.
 */
/** UCX 数据通路参数（原 verbs RDMA 字段已移除）。 */
struct RdmaClientOptions {
  /** Maximum bytes per single transfer; gateway advertises its own limit too. */
  std::size_t max_msg_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};

  /** Maximum idle UCX endpoints kept per gateway (host:ucx_port) bucket. */
  std::size_t pool_max_idle_per_endpoint{8};

  /** Send client-side CRC32C in CommitObject for end-to-end integrity check. */
  bool send_crc32c{false};  // 关闭 CRC32C 以提升性能
};

/**
 * @brief Client instance configuration.
 *
 * Top-level fields are transport-agnostic (endpoint / timeout / logger).
 * Transport-specific knobs live in the nested options (gds / rdma / http)
 * so unrelated paths do not pollute each other's namespace.
 */
struct ClientOptions {
  /** Gateway endpoint in "host:port" form. */
  std::string endpoint;
  /** Identifier reported to the gateway for telemetry and logs. */
  std::string client_id{"us3-turbo-access-client"};
  /** Optional bearer token included in outbound requests. */
  std::string bearer_token;
  /** Headers attached to every outbound HTTP request. */
  std::unordered_map<std::string, std::string> default_headers;
  /** Default per-request timeout when RequestOptions::timeout is unset. */
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};
  /**
   * 端到端单笔传输超时（覆盖 PUT/GET/UploadPart 一次完整操作）。
   * 三通路统一使用：
   *   HTTP : 设到 brpc Controller::set_timeout_ms（覆盖 channel 默认）
   *   RDMA : 在 OpenSession + WRITE 提交 + CQ 等待之间检查 deadline
   *   GDS  : OpenSession 与 PutChunk brpc 调用上 set_timeout_ms
   * 超时统一返回 ErrorCode::kTimeout（retryable=true）。
   *
   * 用作 fallback：若下方按 op 拆分的 *_timeout 为 0 则用 request_timeout。
   */
  std::chrono::milliseconds request_timeout{std::chrono::minutes(5)};

  /**
   * C.3：按 op 拆分超时。0 = 沿用 request_timeout（向后兼容）。
   *
   * 建议用法：
   *   head_timeout: 10s    一次轻量 metadata 调用，5min 过长
   *   put_timeout:  5min   大对象上传保留 5min 默认
   *   get_timeout:  5min   同上
   *
   * 实现见 contracts/request_builder.cpp::MakeRpcContext 按 op 选择。
   */
  std::chrono::milliseconds head_timeout{std::chrono::milliseconds(0)};
  std::chrono::milliseconds put_timeout{std::chrono::milliseconds(0)};
  std::chrono::milliseconds get_timeout{std::chrono::milliseconds(0)};
  /**
   * GDS 数据面（GdsPut）目标 "host:port"，指向独立 backend 进程。
   * 为空则回退到 endpoint，保持旧的单 endpoint 行为（向后兼容）。
   *   endpoint         = 控制面（proxy，OpenSession 走这里）
   *   gds_data_endpoint = 数据面（backend，GdsPut 走这里）
   * 仅 GDS 数据 client 使用此 channel；HTTP/RDMA 通路与 multipart 不受影响。
   */
  std::string gds_data_endpoint;
  /** Data transport selected for transfer operations. */
  DataFlow data_flow{DataFlow::NONE};
  /** Optional logger; if null, the client falls back to a null sink. */
  std::shared_ptr<spdlog::logger> logger;

  /**
   * Worker threads backing the *Async API. Set to 0 to let ClientCore pick
   * a default (hardware_concurrency / 2, minimum 1).
   */
  std::size_t async_worker_threads{0};

  /** GDS-specific tunables. */
  GdsClientOptions  gds;
  /** RDMA-specific tunables. */
  RdmaClientOptions rdma;
  /** HTTP-specific tunables. */
  HttpClientOptions http;
};

}  // namespace us3_turbo::client
