#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <ucp/api/ucp.h>

#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief RDMA(UCX)链路的 client 端内存管理器 + 描述符颁发器。
 *
 * 与 GDS 的 GdsMemoryManager 平行、独立，不共享基类。第一版不抽象。
 *
 * 职责：
 * - 建进程唯一的 UCX context + worker + listener（client 监听，backend 主动 dial）
 * - AcquireDescriptor：对一段 host buffer 做 ucp_mem_map + ucp_rkey_pack，
 *   返回 {remote_addr, rkey_bytes, client_ucx_addr} 供 RdmaPut 透传给 backend
 *
 * 与 Gds 的关键差异：UCX RMA 是连接式，描述符不能像 cuObj DC token 那样
 * 自描述——必须额外带上 client 的 UCX 监听地址，backend 才能主动 dial 建 ep。
 * 故 Descriptor 多一个 client_ucx_addr 字段。
 *
 * 单 worker、UCS_THREAD_MODE_SINGLE：第一版不追求并发。bench 多 worker 共享
 * 同一 Client 时会并发调 AcquireDescriptor，内部用 mu_ 串行化 worker 访问。
 */
class UcxMemoryManager {
 public:
  /** @brief 一次 RDMA PUT 的拉取描述符（随 RdmaPut RPC 透传给 backend）。 */
  struct Descriptor {
    std::uint64_t remote_addr{0};      // host buffer 虚拟地址
    std::string   rkey;                // ucp_rkey_pack 导出的 packed rkey
    std::string   client_ucx_addr;     // client UCX listener "ip:port"
    bool          valid() const noexcept { return remote_addr != 0 && !rkey.empty(); }
  };

  /** @brief 获取进程唯一实例。失败返回 false。 */
  [[nodiscard]] static bool Instance(UcxMemoryManager*& out);

  /**
   * @brief 注册一段 host buffer 并打包 rkey，填充 Descriptor。
   *        同一 ptr 可重复调用（幂等注册，rkey 每次重新 pack）。
   */
  [[nodiscard]] bool AcquireDescriptor(const void* ptr, std::size_t size,
                                       Descriptor& out);

  UcxMemoryManager(const UcxMemoryManager&)            = delete;
  UcxMemoryManager& operator=(const UcxMemoryManager&) = delete;

 private:
  UcxMemoryManager();
  ~UcxMemoryManager();

  [[nodiscard]] bool RegisterBufferUnderLock(void* ptr, std::size_t size);

  // 构造/析构分阶段 init/cleanup：每阶段只负责一个 UCX 组件，构造时
  // 逐阶段推进，任一阶段失败则按反向顺序回滚已完成的阶段。
  [[nodiscard]] bool InitContext();
  [[nodiscard]] bool InitWorker();
  [[nodiscard]] bool InitListener();
  void              StartProgressThread();

  void              CleanupListener();
  void              CleanupWorker();
  void              CleanupContext();

  // listener conn_handler 回调里把新 ep 交给 worker（demo 用法），但本管理器
  // 不持有 ep——backend 主动 dial 建 ep，client 侧的 accept ep 只为完成握手。
  static void ConnCallback(ucp_conn_request_h req, void* arg);

  ucp_context_h  context_{nullptr};
  ucp_worker_h   worker_{nullptr};
  ucp_listener_h listener_{nullptr};
  std::string    listen_addr_;  // "ip:port"，随 Descriptor 透传

  // 后台 progress 线程：client 主线程在 RdmaPut brpc 调用里阻塞等待响应，
  // 没人驱动 worker，listener 的 conn_handler 永不触发 → backend dial 超时。
  // 起一个线程持续 ucp_worker_progress，让 conn_handler 能及时执行。
  std::thread       progress_thread_;
  std::atomic<bool> stop_{false};

  std::mutex                             mu_;  // 串行化单 worker 访问 + 注册表
  std::unordered_map<void*, ucp_mem_h>   registered_;
  bool                                   started_{false};
};

}  // namespace us3_turbo::client
