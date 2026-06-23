#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <cuobjserver.h>

#include "gds/buffer_pool.h"

namespace us3_turbo::backend {

/**
 * @brief GDS 单对象 PUT 数据面接收结果。
 *
 * ok=false 时 error 带失败原因（含 ibv_wc_status 描述）；
 * ok=true 时 bytes_transferred 为实际 RDMA-READ 到 pinned buffer 的字节数，
 * crc32c 为前 transferred 字节的 CRC32C（供端到端校验），字节随后丢弃。
 */
struct DiscardOutcome {
  bool          ok{false};
  std::string   error;
  std::uint64_t bytes_transferred{0};
  std::uint32_t crc32c{0};
};

/**
 * @brief 拥有 cuObjServer + PinnedBufferPool，复刻 gateway GdsExecutor::Start /
 *        PutChunk 的收字节流程，但拉到的字节直接丢弃（不接 IDataStore、不写盘）。
 *
 * 启动顺序：Start() 建 cuObjServer 并校验 isConnected，再建 PinnedBufferPool；
 * 必须在 brpc 开始接受请求前完成，故 ReceiveAndDiscard 无需加锁访问
 * server_/pool_（它们在服务期间恒定不变）。
 */
class BackendGdsSink {
 public:
  BackendGdsSink(std::string bind_host, int rdma_port);
  ~BackendGdsSink();

  BackendGdsSink(const BackendGdsSink&) = delete;
  BackendGdsSink& operator=(const BackendGdsSink&) = delete;

  /** 起 cuObjServer + PinnedBufferPool；失败返回 false。 */
  bool Start();

  /** 先 Shutdown pool（需 server 存活调 deRegisterBuffer），再 reset server。 */
  void Stop();

  [[nodiscard]] bool available() const;

  /**
   * @brief 用客户端 token 反向 RDMA-READ 拉 length 字节进 pinned buffer 后丢弃。
   *
   * length==0 视为成功空传输。length 超过 1 GiB cuObjServer 限制返回失败。
   * object_id 用 "bucket/object_key" 拼（与 gateway BuildObjectId 一致）。
   */
  DiscardOutcome ReceiveAndDiscard(const std::string& object_id,
                                   const std::string& rdma_token,
                                   std::uint64_t length);

 private:
  std::string bind_host_;
  int         rdma_port_;
  std::shared_ptr<cuObjServer> server_;
  std::shared_ptr<us3_turbo::gateway::data_flow::gds::PinnedBufferPool>
      pool_;
};

}  // namespace us3_turbo::backend
