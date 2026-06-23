#pragma once

#include <cstddef>
#include <memory>

#include "us3_turbo/client/options.h"
#include "us3_turbo/client/result.h"
#include "us3_turbo/client/status.h"
#include "us3_turbo/client/types.h"

namespace brpc { class Channel; }  // 前向声明:成员是 unique_ptr<Channel>,无需完整类型

namespace us3_turbo::client {

class MetaRpc;   // 前向声明:成员是 unique_ptr<MetaRpc>,析构带外在 cpp 定义
class ChunkRpc;  // 前向声明:同上

/**
 * @brief Top-level GDS client SDK entry point (client-new).
 *
 * 通路完全分离重构：本 Client 只实现 GDS PUT。不再有 TransferRouter /
 * TransferPath / ClientCore / UploadCoordinator 等抽象 —— 客户端构造时只
 * 实例化 GDS 这一通路,PutObject 直接走 OpenSession → GdsPut。分段上传、
 * HeadObject、GetObject、RDMA 通路均不在本实现内。
 *
 * 内部状态扁平化持有(无 Impl 中间层):channel 所有权已下沉进 MetaRpc /
 * ChunkRpc,这里只持两个 RPC 对象 + 配置/能力。成员为前向声明类型的
 * unique_ptr,析构带外在 client.cpp 定义,故 brpc/protobuf 头不进公共头。
 */
class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();  // 带外在 client.cpp 定义(unique_ptr 成员的完整类型在那里可见)

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** @brief 初始化 brpc channel 与 GDS 组件,幂等。 */
  [[nodiscard]] Result<bool> Initialize();

  /** @brief 释放资源,不可重新初始化。 */
  void Shutdown();

  /** @brief 是否已完成 Initialize。 */
  [[nodiscard]] bool initialized() const;

  /**
   * @brief 通过 GDS 通路上传 @p buffer 的内容为命名对象。
   *
   * 单次 PUT 流程:OpenSession(携带 expected_size) → GdsChunk(GdsPut) →
   * 失败 best-effort AbortSession。
   *
   * @param request 必须设置 bucket / key,GDS PUT 还必须 set_expected_size(>0)。
   * @param buffer  源 buffer,必须 BufferType::kCudaDevice 且在调用返回前有效。
   */
  [[nodiscard]] Result<TransferOutcome> PutObject(const PutObjectRequest& request,
                                                  ConstBufferView buffer) const;

  /**
   * @brief 预注册 GPU device buffer 到 cuObject descriptor 表。
   *
   * 把 cudaMalloc 后的指针一次性 pin 入 BAR1,避免数据面首次传输时
   * nvidia_p2p_get_pages 的毫秒级 syscall 抖动。idempotent。
   */
  [[nodiscard]] Result<bool> RegisterDeviceBuffer(void* ptr, std::size_t size);

  /**
   * @brief 释放 RegisterDeviceBuffer 注册过的 buffer。
   *
   * @warning 必须在 cudaFree(ptr) 之前调用。
   * idempotent:注销未知 ptr 返回 Success。
   */
  [[nodiscard]] Result<bool> UnregisterDeviceBuffer(void* ptr);

 private:
  ClientOptions                  options_;
  std::unique_ptr<MetaRpc>      meta_;     // 内含 → proxy channel
  std::unique_ptr<ChunkRpc>     chunk_;    // 内含 → backend channel
  bool                          initialized_{false};
};

}  // namespace us3_turbo::client
