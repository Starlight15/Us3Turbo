#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace us3_turbo::client {

/**
 * @brief 一次 GDS PUT 尝试的唯一上下文。
 *
 * 取代旧的 OpenSessionRequest / SessionMeta / GdsChunkRequest 三个结构:
 * bucket / key / timeout / request_id / session_id 在整条链路只存这一份,
 * OpenSession 与 GdsChunk 都直接从它读取,不再逐层复制。length 仍来自
 * PutObjectRequest.expected_size,用于 proxy 的 expected_size 校验。
 *
 * timeout 用 request.timeout,非正数时回退到 options.request_timeout;
 * request_id / session_id 由工厂每次生成,故每次重试都会拿到新的 ID。
 */
struct PutAttempt {
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::string bucket;
  std::string key;
  std::optional<std::uint64_t> length;
  std::string request_id;
  std::string session_id;
};

/**
 * @brief OpenSession 成功后回填的会话凭据。
 *
 * 只保留 client 后续实际用到的 ticket(传给 GdsChunk 的 transfer_ticket)。
 * request_id / session_id 已在 PutAttempt 中持有,不再在此重复;proxy 回填
 * 的 gateway_id / expire_at client 从不读取,已移除。
 */
struct SessionGrant {
  std::string ticket;
};

/**
 * @brief GdsPut(GdsChunk)RPC 成功后回填的结果。
 *
 * 只保留 backend 实际填写且 client 后续用到的 etag;bytes_transferred 由
 * buffer.size 推导(单对象 PUT 恒等于 buffer 大小),不随 RPC 回传。
 */
struct GdsPutResult {
  std::string etag;
};

}  // namespace us3_turbo::client
