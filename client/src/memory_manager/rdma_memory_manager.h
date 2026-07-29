#pragma once

// rdma_memory_manager.h — RDMA (libibverbs) 链路的 client 端内存管理器。
//
// RDMA 内存管理器：持有 RDMA CM listener，RegisterBuffer 注册 ibv_mr，
// AcquireDescriptor 产 hex-encoded token 供 RdmaPutChannel 透传。
//
// 反向连接模式：
//   client 创建 listener → backend RdmaQp::Connect 反向连接 → backend RDMA READ。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/src/memory_manager/buffer_registry.h"
#include "client/src/memory_manager/rdma_qp.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief 进程唯一的 RDMA 内存管理器。
 *
 * AcquireDescriptor 对 host buffer 做 ibv_reg_mr + EncodeToken，返回 {token}
 * 供 RdmaPut 透传给 backend。Backend 用 token 里的 ip:port 反向连接 client
 * listener 后 ibv_post_send(RDMA_READ) 拉取数据。
 */
class RdmaMemoryManager : public BufferRegistry<ibv_mr*> {
 public:
  /** @brief 一次 RDMA PUT 的拉取描述符（随 RdmaPut 透传给 backend）。 */
  struct Descriptor {
    std::string token;  // EncodeToken(ip, port, rkey, addr, size)
    bool valid() const noexcept { return !token.empty(); }
  };

  /** @brief 获取进程唯一实例，失败返回 false。
   * @param bind_ip 首次调用时设置 RDMA CM listener 绑定地址；
   *                空字符串默认 "0.0.0.0"（所有接口）。
   *                后续调用忽略该参数。 */
  [[nodiscard]] static bool Instance(RdmaMemoryManager*& out,
                                     const std::string& bind_ip = "");

  /** @brief 注册 host buffer（ibv_reg_mr）并编码 token，填充 Descriptor。幂等。
   * 注册 MR 含 IBV_ACCESS_REMOTE_READ（PUT：backend 拉数据）。 */
  [[nodiscard]] bool AcquireDescriptor(const void* ptr, std::size_t size, Descriptor& out);

  /** @brief 注册 host buffer 用于 GET（backend RDMA WRITE 推数据到 client buffer）。
   * 注册 MR 含 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE。 */
  [[nodiscard]] bool AcquireDescriptorForWrite(const void* ptr, std::size_t size, Descriptor& out);

  RdmaMemoryManager(const RdmaMemoryManager&) = delete;
  RdmaMemoryManager& operator=(const RdmaMemoryManager&) = delete;

 private:
  RdmaMemoryManager();

  ~RdmaMemoryManager() override;

  // BufferRegistry<ibv_mr*> 钩子：真正 ibv_reg_mr / ibv_dereg_mr。
  [[nodiscard]] bool DoRegister(void* ptr, std::size_t size, ibv_mr*& out) override;

  void DoUnregister(void* ptr, ibv_mr*& handle) override;

  /** @brief 公共的 descriptor 获取骨架：幂等注册 buffer → 编码 token。
   * access_flags 控制 ibv_reg_mr 的 access 参数：
   *   - PUT 用 IBV_ACCESS_REMOTE_READ
   *   - GET 用 IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE
   */
  [[nodiscard]] bool AcquireDescriptorImpl(const void* ptr, std::size_t size,
                                           int access_flags, const char* tag,
                                           Descriptor& out);

  /** @brief 创建 listener 并建立 PD。 */
  [[nodiscard]] bool InitListener();

  /** @brief 启动后台 accept 线程处理 backend 反向连接。 */
  void StartAcceptThread();

  /** @brief 后台 accept 循环：listener_->Accept()，暂存 QP 保持存活。 */
  void AcceptLoop();

  /** @brief 逆序 cleanup。 */
  void CleanupListener();

  RdmaQp* listener_;      // RDMA CM listener
  ibv_pd* pd_;            // 从 listener 设备分配，供 MR 注册 + Accept 复用
  std::string listen_ip_;
  std::uint16_t listen_port_;

  // 后台 accept 线程：backend 做 Connect 时触发 CONNECT_REQUEST → Accept 创建 QP。
  std::thread accept_thread_;
  std::atomic<bool> stop_{false};

  // 已接受的 QP 缓存：QP 须在 backend 读期间保持存活。
  mutable std::mutex qp_mu_;
  std::vector<RdmaQp*> accepted_qps_;

  bool started_{false};
};

}  // namespace us3_turbo::client