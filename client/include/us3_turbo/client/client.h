#pragma once

#include <cstddef>
#include <memory>

#include "us3_turbo/client/options.h"
#include "us3_turbo/client/result.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class MetaRpc;
class ChunkRpc;
class GdsMemoryManager;

/** @brief GDS-only client SDK 入口。只实现 GDS PUT 链路：OpenSession → GdsPut。 */
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
  GdsMemoryManager*             gds_mgr_{nullptr};  // Initialize 缓存,避免重复 Instance()
  bool                          initialized_{false};
};

}  // namespace us3_turbo::client
