#pragma once

#include <cstddef>

#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class MetaRpc;
class ChunkRpc;
class GdsMemoryManager;

class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** @brief 初始化 brpc channel 与 GDS 组件，幂等。返回 true 成功，false 失败。 */
  [[nodiscard]] bool Initialize();

  void Shutdown();

  [[nodiscard]] bool initialized() const;

  /**
   * @brief 通过 GDS 通路上传 @p buffer。返回 true 成功，false 失败。
   * @param out 成功时填充 TransferOutcome。
   */
  [[nodiscard]] bool PutObject(const PutObjectRequest& request,
                              ConstBufferView buffer,
                              TransferOutcome& out) const;

  [[nodiscard]] bool RegisterDeviceBuffer(void* ptr, std::size_t size);
  [[nodiscard]] bool UnregisterDeviceBuffer(void* ptr);

 private:
  ClientOptions              options_;
  std::unique_ptr<MetaRpc>   meta_;
  std::unique_ptr<ChunkRpc>  chunk_;
  GdsMemoryManager*          gds_mgr_{nullptr};
  bool                       initialized_{false};
};

}  // namespace us3_turbo::client
