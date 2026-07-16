#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include <ucp/api/ucp.h>

#include "client/src/memory_manager/buffer_registry.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief UCX 链路(底层走 RDMA)的 client 端内存管理器。
 *
 * 进程唯一的 UCX context/worker/listener;AcquireDescriptor 对 host buffer 做
 * ucp_mem_map + ucp_rkey_pack,返回 {remote_addr, rkey, client_ucx_addr} 供
 * UcxPut 透传给 backend。与 GDS 的差异:UCX RMA 是连接式,描述符须额外带
 * client 监听地址,backend 才能 dial 建 ep。
 */
class UcxMemoryManager : public BufferRegistry<ucp_mem_h> {
 public:
  /** @brief 一次 UCX PUT 的拉取描述符(随 UcxPut 透传给 backend)。 */
  struct Descriptor {
    std::uint64_t remote_addr{0};  // host buffer 虚拟地址
    std::string rkey;              // ucp_rkey_pack 导出的 packed rkey
    std::string client_ucx_addr;   // client UCX listener "ip:port"
    bool valid() const noexcept { return remote_addr != 0 && !rkey.empty(); }
  };

  /** @brief 获取进程唯一实例,失败返回 false。 */
  [[nodiscard]] static bool Instance(UcxMemoryManager*& out);

  /** @brief 注册 host buffer 并打包 rkey,填充 Descriptor。幂等。 */
  [[nodiscard]] bool AcquireDescriptor(const void* ptr, std::size_t size,
                                       Descriptor& out);

  UcxMemoryManager(const UcxMemoryManager&) = delete;
  UcxMemoryManager& operator=(const UcxMemoryManager&) = delete;

 private:
  UcxMemoryManager();
  ~UcxMemoryManager() override;

  // BufferRegistry<ucp_mem_h> 钩子:真正 ucp_mem_map / ucp_mem_unmap。
  [[nodiscard]] bool DoRegister(void* ptr, std::size_t size, ucp_mem_h& out) override;
  void DoUnregister(void* ptr, ucp_mem_h& handle) override;

  /** @brief 分阶段 init:任一失败按反向顺序回滚。 */
  [[nodiscard]] bool InitContext();
  /** @brief 创建 worker(MULTI,跨线程安全)。 */
  [[nodiscard]] bool InitWorker();
  /** @brief 创建 listener 并 query 取回实际绑定地址。 */
  [[nodiscard]] bool InitListener();
  /** @brief 启动后台 progress 线程驱动 conn_handler。 */
  void StartProgressThread();

  /** @brief 逆序 cleanup 各阶段组件,幂等(nullptr 跳过)。 */
  void CleanupListener();
  void CleanupWorker();
  void CleanupContext();

  /** @brief listener conn_handler:accept 新 ep 完成握手,client 不持有 ep。 */
  static void ConnCallback(ucp_conn_request_h req, void* arg);

  ucp_context_h context_{nullptr};
  ucp_worker_h worker_{nullptr};
  ucp_listener_h listener_{nullptr};
  std::string listen_addr_;  // "ip:port",随 Descriptor 透传

  // 后台 progress 线程驱动 listener conn_handler,否则主线程阻塞时 backend dial
  // 超时。
  std::thread progress_thread_;
  std::atomic<bool> stop_{false};

  bool started_{false};
};

}  // namespace us3_turbo::client
