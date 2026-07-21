#pragma once

// rdma_qp.h — libibverbs RC QP 封装（RDMA CM / librdmacm），client 侧。
//
// client 仅需 listener（CreateListener + Accept）和 EncodeToken。
// backend 用 ufile-ac/rdma/ 中的镜像副本做 Connect + PostRead + DecodeToken。
//
// Token 格式（hex 编码的二进制，须与 ufile-ac/rdma/rdma_qp.h 一致）：
//   ip_len(2B) | ip_str | port(2B) | rkey(4B) | addr(8B) | size(8B)

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace us3_turbo::client {

/** @brief RDMA RC QP 配置参数。create_qp / rdma_accept 使用。 */
struct RdmaQpConfig {
  int max_send_wr{16};
  int max_recv_wr{16};
  int max_rd_atomic{16};
  int cq_size{128};
  int retry_cnt{7};
  int rnr_retry{7};
};

class RdmaQp {
 public:
  /** @brief 创建绑定 ip:port 的 listener。port=0 由系统分配。
   *  失败返回 nullptr。 */
  static RdmaQp* CreateListener(const char* ip, std::uint16_t port);

  /** @brief 接受一个 RDMA CM 连接。阻塞直到 client 连接或 timeout_ms 超时。
   *  返回已连接的 RdmaQp，失败/超时返回 nullptr。
   *
   *  @param external_pd 若非 nullptr，创建 QP 时复用此 PD（须从本 listener 的
   *  device() 获取），保证 MR 和 QP 共用同一 PD。 */
  RdmaQp* Accept(int timeout_ms, ibv_pd* external_pd = nullptr,
                 const RdmaQpConfig& config = RdmaQpConfig{});

  /** @brief 返回底层 RDMA 设备的 verbs context。 */
  ibv_context* device() const {
    if (listen_id_) return listen_id_->verbs;
    if (cm_id_) return cm_id_->verbs;
    return nullptr;
  }

  std::uint16_t listen_port() const { return listen_port_; }
  bool is_listener() const { return listen_id_ != nullptr; }
  bool connected() const { return connected_; }
  ibv_pd* pd() const { return pd_; }

  ~RdmaQp();

  RdmaQp(const RdmaQp&) = delete;
  RdmaQp& operator=(const RdmaQp&) = delete;

 private:
  RdmaQp();

  bool WaitEvent(rdma_cm_event_type expected, rdma_cm_event*& out_event,
                 int timeout_ms);

  void Cleanup();

  rdma_cm_id* listen_id_;
  rdma_event_channel* listen_ec_;
  std::uint16_t listen_port_;

  rdma_cm_id* cm_id_;
  rdma_event_channel* cm_ec_;  // accept 后迁移到的独立事件通道
  ibv_pd* pd_;
  ibv_cq* cq_;
  std::uint32_t qp_num_;
  bool connected_;
  bool owns_pd_;
};

/** @brief 编码 listener 地址 + MR 描述符为 hex 字符串，供 proxy 透传。
 *  二进制布局须与 ufile_ac::rdma::EncodeToken 完全一致。 */
std::string EncodeToken(const char* ip, std::uint16_t port,
                        std::uint32_t rkey, std::uint64_t addr,
                        std::uint64_t size);

}  // namespace us3_turbo::client