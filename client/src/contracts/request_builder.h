#pragma once

#include <chrono>
#include <string_view>

#include "client/src/common/rpc_context.h"
#include "client/src/contracts/requests.h"
#include "control_plane.pb.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief client-new 的请求 / 会话 / 结果装配工厂。
 *
 * 旧 client 用 SessionPlan / ChunkOpPlan / SessionOpening / ChunkOp 一串
 * 中转结构;client-new 把它们折叠成三个函数,直接以 PutObjectRequest +
 * buffer 为唯一信息源,逐层装配 OpenSessionRequest → GdsChunkRequest →
 * TransferOutcome,字段从源头复制一次,不再手写中间结构。
 *
 * 单对象 GDS PUT 的固定语义(op=kPut、data_flow=GPUDirect、chunk_offset=0、
 * chunk_size=buffer.size)直接由工厂钉死,调用侧无需重复填写。
 */

/** @brief 装配 OpenSessionRequest:bucket/key/length/timeout 来自 PutObjectRequest。 */
[[nodiscard]] OpenSessionRequest MakeOpenSessionRequest(const ClientOptions& options,
                                                        const PutObjectRequest& request);

/** @brief 从 OpenSession 响应回填会话元数据。 */
[[nodiscard]] SessionMeta ImportSession(
    const us3_turbo::proxy::OpenSessionResponse& response);

/**
 * @brief 装配 GdsChunkRequest:复用 OpenSessionRequest 的 context / bucket /
 *        key,补 token / session / ticket。
 */
[[nodiscard]] GdsChunkRequest MakeGdsChunkRequest(const OpenSessionRequest& open,
                                                  const SessionMeta& session,
                                                  ConstBufferView buffer,
                                                  std::string_view rdma_token);

/**
 * @brief 从 GdsChunk 响应装配 TransferOutcome:只回填 etag(backend 唯一填写)
 *        + bytes_transferred + 会话 request_id / session_id。
 */
[[nodiscard]] TransferOutcome MakeTransferOutcome(const SessionMeta& session,
                                                  const us3_turbo::proxy::GdsChunkResponse& response,
                                                  ConstBufferView buffer);

}  // namespace us3_turbo::client
