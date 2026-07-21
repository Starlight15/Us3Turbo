// rdma_qp.cpp — libibverbs RDMA CM listener（client 侧）。
// Token 格式须与 ufile-ac/rdma/rdma_qp.cc 完全一致。

#include "client/src/memory_manager/rdma_qp.h"

#include <arpa/inet.h>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace us3_turbo::client {

RdmaQp::RdmaQp()
    : listen_id_(nullptr),
      listen_ec_(nullptr),
      listen_port_(0),
      cm_id_(nullptr),
      cm_ec_(nullptr),
      pd_(nullptr),
      cq_(nullptr),
      qp_num_(0),
      connected_(false),
      owns_pd_(true) {}

RdmaQp::~RdmaQp() { Cleanup(); }

void RdmaQp::Cleanup() {
  if (cq_) {
    ibv_destroy_cq(cq_);
    cq_ = nullptr;
  }
  if (pd_ && owns_pd_) {
    ibv_dealloc_pd(pd_);
    pd_ = nullptr;
  }
  if (cm_id_) {
    rdma_destroy_id(cm_id_);
    cm_id_ = nullptr;
  }
  if (cm_ec_) {
    rdma_destroy_event_channel(cm_ec_);
    cm_ec_ = nullptr;
  }
  if (listen_id_) {
    rdma_destroy_id(listen_id_);
    listen_id_ = nullptr;
  }
  if (listen_ec_) {
    rdma_destroy_event_channel(listen_ec_);
    listen_ec_ = nullptr;
  }
}

RdmaQp* RdmaQp::CreateListener(const char* ip, std::uint16_t port) {
  auto* me = new RdmaQp();
  if (me == nullptr) return nullptr;

  me->listen_ec_ = rdma_create_event_channel();
  if (me->listen_ec_ == nullptr) {
    delete me;
    return nullptr;
  }

  if (rdma_create_id(me->listen_ec_, &me->listen_id_, nullptr,
                     RDMA_PS_TCP) != 0) {
    delete me;
    return nullptr;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    delete me;
    return nullptr;
  }

  if (rdma_bind_addr(me->listen_id_, reinterpret_cast<sockaddr*>(&addr)) != 0) {
    delete me;
    return nullptr;
  }

  me->listen_port_ = ntohs(rdma_get_src_port(me->listen_id_));

  if (rdma_listen(me->listen_id_, 0) != 0) {
    delete me;
    return nullptr;
  }

  return me;
}

bool RdmaQp::WaitEvent(rdma_cm_event_type expected,
                       rdma_cm_event*& out_event, int timeout_ms) {
  // 按优先级选取事件通道：listener / accepted / cm_id 默认通道。
  rdma_event_channel* ec = listen_ec_ != nullptr ? listen_ec_
                          : cm_ec_ != nullptr     ? cm_ec_
                          : cm_id_ != nullptr     ? cm_id_->channel
                                                  : nullptr;
  if (ec == nullptr) return false;

  int total_ms = 0;
  while (total_ms < timeout_ms || timeout_ms < 0) {
    int fd = ec->fd;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    timeval tv{};
    int wait_ms = timeout_ms < 0 ? 100 : (timeout_ms - total_ms);
    if (wait_ms > 100) wait_ms = 100;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;

    int ret = select(fd + 1, &fds, nullptr, nullptr, &tv);
    if (ret < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (ret == 0) {
      total_ms += wait_ms;
      if (timeout_ms >= 0 && total_ms >= timeout_ms) return false;
      continue;
    }

    rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(ec, &event) != 0) return false;

    if (event->event == expected) {
      out_event = event;
      return true;
    }

    // 非预期事件：REJECTED / DISCONNECTED / DEVICE_REMOVAL 为致命错误；
    // 其他事件（如 listener 上收到 ADDR_RESOLVED）ack 后继续轮询。
    rdma_cm_event_type unexpected = event->event;
    rdma_ack_cm_event(event);
    if (unexpected == RDMA_CM_EVENT_REJECTED ||
        unexpected == RDMA_CM_EVENT_DISCONNECTED ||
        unexpected == RDMA_CM_EVENT_DEVICE_REMOVAL) {
      return false;
    }
    total_ms += wait_ms;
  }
  return false;
}

RdmaQp* RdmaQp::Accept(int timeout_ms, ibv_pd* external_pd, const RdmaQpConfig& config) {
  if (listen_id_ == nullptr) return nullptr;

  rdma_cm_event* event = nullptr;
  if (!WaitEvent(RDMA_CM_EVENT_CONNECT_REQUEST, event, timeout_ms)) {
    return nullptr;
  }

  rdma_cm_id* new_id = event->id;
  rdma_ack_cm_event(event);

  // 迁移到独立事件通道，避免 ESTABLISHED/DISCONNECTED 与 CONNECT_REQUEST 冲突。
  rdma_event_channel* new_ec = rdma_create_event_channel();
  if (new_ec == nullptr) {
    rdma_destroy_id(new_id);
    return nullptr;
  }
  if (rdma_migrate_id(new_id, new_ec) != 0) {
    rdma_destroy_event_channel(new_ec);
    rdma_destroy_id(new_id);
    return nullptr;
  }

  auto* qp = new RdmaQp();
  if (qp == nullptr) {
    rdma_destroy_event_channel(new_ec);
    rdma_destroy_id(new_id);
    return nullptr;
  }
  qp->cm_ec_ = new_ec;

  ibv_context* ctx = new_id->verbs;

  // 优先复用外部 PD（保证 MR 和 QP 共用同一 PD），否则自建。
  qp->pd_ = external_pd ? external_pd : ibv_alloc_pd(ctx);
  if (qp->pd_ == nullptr) {
    delete qp;
    rdma_destroy_id(new_id);
    return nullptr;
  }
  bool owns_pd = (external_pd == nullptr);

  qp->cq_ = ibv_create_cq(ctx, config.cq_size, nullptr, nullptr, 0);
  if (qp->cq_ == nullptr) {
    if (owns_pd) { ibv_dealloc_pd(qp->pd_); qp->pd_ = nullptr; }
    delete qp;
    rdma_destroy_id(new_id);
    return nullptr;
  }

  ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq = qp->cq_;
  qp_attr.recv_cq = qp->cq_;
  qp_attr.cap.max_send_wr = config.max_send_wr;
  qp_attr.cap.max_recv_wr = config.max_recv_wr;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;
  qp_attr.cap.max_inline_data = 0;
  qp_attr.qp_type = IBV_QPT_RC;

  if (rdma_create_qp(new_id, qp->pd_, &qp_attr) != 0) {
    if (owns_pd) { ibv_dealloc_pd(qp->pd_); qp->pd_ = nullptr; }
    delete qp;
    rdma_destroy_id(new_id);
    return nullptr;
  }

  qp->cm_id_ = new_id;
  qp->qp_num_ = new_id->qp->qp_num;
  qp->owns_pd_ = owns_pd;

  // modify QP → INIT
  {
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = new_id->port_num;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(new_id->qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) != 0) {
      qp->cm_id_ = nullptr;  // Prevent Cleanup() from destroying; destroyed below
      delete qp;
      rdma_destroy_id(new_id);
      return nullptr;
    }
  }

  rdma_conn_param conn_param{};
  conn_param.responder_resources = config.max_rd_atomic;
  conn_param.initiator_depth = config.max_rd_atomic;
  conn_param.retry_count = config.retry_cnt;
  conn_param.rnr_retry_count = config.rnr_retry;

  if (rdma_accept(new_id, &conn_param) != 0) {
    qp->cm_id_ = nullptr;  // Prevent Cleanup() from destroying; destroyed below
    delete qp;
    rdma_destroy_id(new_id);
    return nullptr;
  }

  if (!qp->WaitEvent(RDMA_CM_EVENT_ESTABLISHED, event, timeout_ms)) {
    delete qp;
    return nullptr;
  }
  rdma_ack_cm_event(event);

  qp->connected_ = true;
  return qp;
}

// ===========================================================================
// Token 编码（须与 ufile-ac/rdma/rdma_qp.cc EncodeToken 完全一致）
// ===========================================================================

std::string EncodeToken(const char* ip, std::uint16_t port,
                        std::uint32_t rkey, std::uint64_t addr,
                        std::uint64_t size) {
  std::string ip_str(ip);
  std::uint16_t ip_len = static_cast<std::uint16_t>(ip_str.size());

  const std::size_t bin_size = 2 + ip_len + 2 + 4 + 8 + 8;
  std::vector<unsigned char> bin(bin_size);
  std::size_t off = 0;

  auto put_u16 = [&](std::uint16_t v) {
    bin[off++] = static_cast<unsigned char>(v & 0xff);
    bin[off++] = static_cast<unsigned char>((v >> 8) & 0xff);
  };
  auto put_u32 = [&](std::uint32_t v) {
    bin[off++] = static_cast<unsigned char>(v & 0xff);
    bin[off++] = static_cast<unsigned char>((v >> 8) & 0xff);
    bin[off++] = static_cast<unsigned char>((v >> 16) & 0xff);
    bin[off++] = static_cast<unsigned char>((v >> 24) & 0xff);
  };
  auto put_u64 = [&](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      bin[off++] = static_cast<unsigned char>((v >> (i * 8)) & 0xff);
    }
  };

  put_u16(ip_len);
  std::memcpy(&bin[off], ip_str.data(), ip_len);
  off += ip_len;
  put_u16(port);
  put_u32(rkey);
  put_u64(addr);
  put_u64(size);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (auto b : bin) {
    oss << std::setw(2) << static_cast<int>(b);
  }
  return oss.str();
}

}  // namespace us3_turbo::client