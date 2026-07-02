#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string>
#include <thread>

#include <ucp/api/ucp.h>

#include "client/src/memory_manager/buffer_registry.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief UCX 链路(底层走 RDMA)的 client 端内存管理器 + 描述符颁发器。
 *
 * 与 GdsMemoryManager 平行、独立。注册表/锁/幂等流程继承自
 * BufferRegistry<ucp_mem_h>;ucp_mem_map/unmap 在 DoRegister/DoUnregister 里。
 * AcquireDescriptor / Descriptor 留在本类。
 *
 * 职责:
 * - 建进程唯一的 UCX context + worker + listener(client 监听,backend 主动 dial)
 * - AcquireDescriptor:对 host buffer 做 ucp_mem_map + ucp_rkey_pack,返回
 *   {remote_addr, rkey_bytes, client_ucx_addr} 供 UcxPut 透传给 backend
 *
 * 与 GDS 的关键差异:UCX RMA 是连接式,描述符不能像 cuObj DC token 那样自描述
 * ——必须额外带 client 的 UCX 监听地址,backend 才能 dial 建 ep。
 *
 * 单 worker + UCS_THREAD_MODE_MULTI:bench 多 worker 共享 Client 时并发调
 * AcquireDescriptor,且 listener conn_handler 在 UCX 内部线程触发。基类 mu_
 * 串行化注册表访问。
 */
class UcxMemoryManager : public BufferRegistry<ucp_mem_h> {
 public:
  /** @brief 一次 UCX PUT 的拉取描述符(随 UcxPut RPC 透传给 backend)。 */
  struct Descriptor {
    std::uint64_t remote_addr{0};      // host buffer 虚拟地址
    std::string   rkey;                // ucp_rkey_pack 导出的 packed rkey
    std::string   client_ucx_addr;     // client UCX listener "ip:port"
    bool          valid() const noexcept { return remote_addr != 0 && !rkey.empty(); }
  };

  /** @brief 获取进程唯一实例,失败返回 false。 */
  [[nodiscard]] static bool Instance(UcxMemoryManager*& out);

  /**
   * @brief 注册 host buffer 并打包 rkey,填充 Descriptor。
   *        同一 ptr 可重复调用(幂等注册,rkey 每次重新 pack)。
   */
  [[nodiscard]] bool AcquireDescriptor(const void* ptr, std::size_t size,
                                       Descriptor& out);

  UcxMemoryManager(const UcxMemoryManager&)            = delete;
  UcxMemoryManager& operator=(const UcxMemoryManager&) = delete;

 private:
  UcxMemoryManager();
  ~UcxMemoryManager() override;

  // BufferRegistry<ucp_mem_h> 钩子:真正 ucp_mem_map / ucp_mem_unmap。
  [[nodiscard]] bool DoRegister(void* ptr, std::size_t size,
                                ucp_mem_h& out) override;
  void DoUnregister(void* ptr, ucp_mem_h& handle) override;

  /** @brief 分阶段 init:每阶段一个 UCX 组件,任一失败按反向顺序回滚。 */
  [[nodiscard]] bool InitContext();
  /** @brief 创建 worker(UCS_THREAD_MODE_MULTI,跨线程安全)。 */
  [[nodiscard]] bool InitWorker();
  /** @brief 创建 listener 并 query 取回实际绑定地址。 */
  [[nodiscard]] bool InitListener();
  /** @brief 启动后台 progress 线程驱动 listener conn_handler。 */
  void              StartProgressThread();

  /** @brief 逆序 cleanup 各阶段组件,幂等(nullptr 跳过)。 */
  void              CleanupListener();
  void              CleanupWorker();
  void              CleanupContext();

  /**
   * @brief listener conn_handler:把新 ep 交给 worker。本管理器不持有 ep——
   *        backend 主动 dial 建 ep,client 侧 accept ep 只为完成握手。
   */
  static void ConnCallback(ucp_conn_request_h req, void* arg);

  ucp_context_h  context_{nullptr};
  ucp_worker_h   worker_{nullptr};
  ucp_listener_h listener_{nullptr};
  std::string    listen_addr_;  // "ip:port",随 Descriptor 透传

  // 后台 progress 线程:主线程在 UcxPut brpc 调用里阻塞,无人驱动 worker,
  // listener conn_handler 永不触发 → backend dial 超时。此线程持续 progress。
  std::thread       progress_thread_;
  std::atomic<bool> stop_{false};

  bool                                   started_{false};
};

}  // namespace us3_turbo::client
