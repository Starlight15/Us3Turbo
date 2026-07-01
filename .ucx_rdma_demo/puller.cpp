// puller.cpp — UCX RDMA demo 的 backend 角色（puller）。
//
// 机制证明目标：puller 主动 dial source(client) 建 ucp_ep_h，在 ep 上
// unpack source 发来的 rkey，然后用 ucp_get_nbx 反向把 source 的 host
// 内存拉进自己 malloc 的 buffer，逐字节校验。这复刻 Us3Turbo RDMA PUT
// 链路里 backend 用 ucp_get_nbx 后端拉取 client CPU 内存的核心机制。
//
// 用法：./puller <source_ip> <source_port> [bytes=1048576]
// 例：  ./puller 192.168.1.198 39321 1048576

#include <ucp/api/ucp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// source 发给 puller 的拉取描述符：remote_addr + length + packed rkey。
// 固定大小、内嵌 512B rkey 槽，一次 ucp_tag_recv_nbx 收齐。
// （rc_mlx5 的 packed rkey 远小于 512B。）
struct PullDesc {
  std::uint64_t remote_addr;   // source 侧 host buffer 的虚拟地址
  std::uint64_t length;        // 待拉取字节数
  std::uint64_t rkey_size;     // rkey_buf 里实际有效字节数
  unsigned char rkey_buf[512];
};

constexpr std::uint64_t kTagDesc = 1;
constexpr std::uint64_t kTagDone = 2;

// 等待一个 ucp_tag_recv_nbx 的请求完成：progress 直到 UCS_INPROGRESS 消失。
// 成功填 *status=UCS_OK 返回 true；请求失败/出错返回 false。
bool WaitRecv(ucp_worker_h worker, void* req) {
  if (req == nullptr) {
    // 内联完成（不可能发生在 recv，但防御性处理）。
    return true;
  }
  if (UCS_PTR_IS_ERR(req)) {
    std::fprintf(stderr, "puller: recv posted with error: %s\n",
                 ucs_status_string(UCS_PTR_STATUS(req)));
    ucp_request_free(req);
    return false;
  }
  while (ucp_request_check_status(req) == UCS_INPROGRESS) {
    ucp_worker_progress(worker);
  }
  ucs_status_t st = ucp_request_check_status(req);
  ucp_request_free(req);
  if (st != UCS_OK) {
    std::fprintf(stderr, "puller: recv completed with error: %s\n",
                 ucs_status_string(st));
    return false;
  }
  return true;
}

// 等待一个 get/send 请求完成（与 WaitRecv 同形）。
bool WaitOp(ucp_worker_h worker, void* req) {
  if (req == nullptr) return true;  // 内联完成（get/send 可能内联完成）。
  if (UCS_PTR_IS_ERR(req)) {
    std::fprintf(stderr, "puller: op posted with error: %s\n",
                 ucs_status_string(UCS_PTR_STATUS(req)));
    ucp_request_free(req);
    return false;
  }
  while (ucp_request_check_status(req) == UCS_INPROGRESS) {
    ucp_worker_progress(worker);
  }
  ucs_status_t st = ucp_request_check_status(req);
  ucp_request_free(req);
  if (st != UCS_OK) {
    std::fprintf(stderr, "puller: op completed with error: %s\n",
                 ucs_status_string(st));
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <source_ip> <source_port> [bytes=1048576]\n",
                 argv[0]);
    return 2;
  }
  const std::string src_ip = argv[1];
  const int src_port = std::atoi(argv[2]);
  // puller 不预知字节数（由 source 在 desc 里告知），argv[3] 仅占位以对齐 run.sh。

  // ---- 1. context + worker（features: RMA 给 get_nbx，TAG 给 desc 收发）----
  ucp_params_t ctx_params{};
  ctx_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  ctx_params.features = UCP_FEATURE_RMA | UCP_FEATURE_TAG;
  ucp_config_t* config = nullptr;
  if (ucp_config_read(nullptr, nullptr, &config) != UCS_OK) {
    std::fprintf(stderr, "puller: ucp_config_read failed\n");
    return 1;
  }
  ucp_context_h context = nullptr;
  ucs_status_t st = ucp_init(&ctx_params, config, &context);
  ucp_config_release(config);
  if (st != UCS_OK) {
    std::fprintf(stderr, "puller: ucp_init failed: %s\n",
                 ucs_status_string(st));
    return 1;
  }

  ucp_worker_params_t wparams{};
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wparams.thread_mode = UCS_THREAD_MODE_SINGLE;
  ucp_worker_h worker = nullptr;
  st = ucp_worker_create(context, &wparams, &worker);
  if (st != UCS_OK) {
    std::fprintf(stderr, "puller: ucp_worker_create failed: %s\n",
                 ucs_status_string(st));
    ucp_cleanup(context);
    return 1;
  }

  // ---- 2. ep_create(SOCK_ADDR) 主动 dial source ----
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(src_port));
  if (inet_pton(AF_INET, src_ip.c_str(), &addr.sin_addr) != 1) {
    std::fprintf(stderr, "puller: invalid source ip '%s'\n", src_ip.c_str());
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  ucp_ep_params_t ep_params{};
  // SOCK_ADDR 客户端-服务端连接建立必须同时置 CLIENT_SERVER flag，否则
  // ucp_ep_create 报 EINVAL。
  ep_params.field_mask =
      UCP_EP_PARAM_FIELD_SOCK_ADDR | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
      UCP_EP_PARAM_FIELD_FLAGS;
  ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  ep_params.err_mode = UCP_ERR_HANDLING_MODE_NONE;
  ep_params.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);
  ep_params.sockaddr.addrlen = sizeof(addr);
  ucp_ep_h ep = nullptr;
  st = ucp_ep_create(worker, &ep_params, &ep);
  if (st != UCS_OK) {
    std::fprintf(stderr, "puller: ucp_ep_create failed: %s\n",
                 ucs_status_string(st));
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  std::printf("puller: dialed ep to %s:%d\n", src_ip.c_str(), src_port);

  // ---- 3. tag_recv 收 PullDesc ----
  ucp_rkey_h rkey = nullptr;  // 提前声明，避免 goto 跨初始化。
  {
    PullDesc desc{};
    ucp_request_param_t recv_param{};
    void* recv_req = ucp_tag_recv_nbx(worker, &desc, sizeof(desc), kTagDesc,
                                      0xFFFFFFFFFFFFFFFFULL, &recv_param);
    if (!WaitRecv(worker, recv_req)) {
      goto cleanup_ep;
    }
    if (desc.rkey_size > sizeof(desc.rkey_buf)) {
      std::fprintf(stderr, "puller: rkey_size=%llu too large\n",
                   static_cast<unsigned long long>(desc.rkey_size));
      goto cleanup_ep;
    }
    std::printf("puller: recv desc remote_addr=0x%llx length=%llu rkey_size=%llu\n",
                static_cast<unsigned long long>(desc.remote_addr),
                static_cast<unsigned long long>(desc.length),
                static_cast<unsigned long long>(desc.rkey_size));

    // ---- 4. rkey_unpack（在 dial 出的 ep 上 unpack source 的 packed rkey）----
    st = ucp_ep_rkey_unpack(ep, desc.rkey_buf, &rkey);
    if (st != UCS_OK) {
      std::fprintf(stderr, "puller: ucp_ep_rkey_unpack failed: %s\n",
                   ucs_status_string(st));
      goto cleanup_ep;
    }

    // ---- 5. ucp_get_nbx 拉取 ----
    void* buf = std::malloc(static_cast<size_t>(desc.length));
    if (buf == nullptr) {
      std::fprintf(stderr, "puller: malloc %llu bytes failed\n",
                   static_cast<unsigned long long>(desc.length));
      ucp_rkey_destroy(rkey); rkey = nullptr;
      goto cleanup_ep;
    }
    auto t0 = std::chrono::steady_clock::now();
    ucp_request_param_t get_param{};
    void* get_req =
        ucp_get_nbx(ep, buf, static_cast<size_t>(desc.length),
                    desc.remote_addr, rkey, &get_param);
    if (!WaitOp(worker, get_req)) {
      std::free(buf);
      ucp_rkey_destroy(rkey); rkey = nullptr;
      goto cleanup_ep;
    }
    auto t1 = std::chrono::steady_clock::now();
    const double sec =
        std::chrono::duration<double>(t1 - t0).count();
    const double gbps =
        (sec > 0.0) ? (static_cast<double>(desc.length) / (1e9 * sec)) : 0.0;

    // ---- 6. 逐字节校验：期望 buf[i] == i & 0xFF ----
    bool ok = true;
    const unsigned char* p = static_cast<const unsigned char*>(buf);
    for (std::uint64_t i = 0; i < desc.length; ++i) {
      if (p[i] != (i & 0xFF)) { ok = false; break; }
    }
    if (ok) {
      std::printf(
          "puller: OK bytes=%llu latency=%.3fms throughput=%.2fGB/s pattern-verified\n",
          static_cast<unsigned long long>(desc.length), sec * 1000.0, gbps);
    } else {
      std::fprintf(stderr, "puller: PATTERN MISMATCH (data corrupted)\n");
    }

    // ---- 7. 回 source 一个 "done" ----
    std::uint64_t done = 1;
    ucp_request_param_t send_param{};
    void* send_req = ucp_tag_send_nbx(ep, &done, sizeof(done), kTagDone, &send_param);
    WaitOp(worker, send_req);  // best-effort

    std::free(buf);
    ucp_rkey_destroy(rkey); rkey = nullptr;
    // ok=false 时落到 cleanup_ep；仍正常清理 ep。
  }

cleanup_ep:
  if (rkey != nullptr) {
    ucp_rkey_destroy(rkey);
  }
  {
    // ep_close_nbx(flush) best-effort
    ucp_request_param_t close_param{};
    void* close_req = ucp_ep_close_nbx(ep, &close_param);
    if (close_req != nullptr && !UCS_PTR_IS_ERR(close_req)) {
      while (ucp_request_check_status(close_req) == UCS_INPROGRESS) {
        ucp_worker_progress(worker);
      }
      ucp_request_free(close_req);
    }
  }
  ucp_worker_destroy(worker);
  ucp_cleanup(context);
  return 0;
}
