// source.cpp — UCX RDMA demo 的 client 角色（source）。
//
// 机制证明目标：source 建一个 UCX listener，注册一段 host 内存并打包 rkey，
// 在 puller(backend) 主动 dial 进来建好的 ep 上，把 {remote_addr, length, rkey}
// 用 ucp_tag_send_nbx 发给 puller；puller 据此 ucp_get_nbx 反向拉取。
// 这复刻 Us3Turbo RDMA PUT 链路(B2) 里 client 暴露 UCX 监听地址 + 描述符、
// backend 主动 dial 的核心机制（demo 里用 tag_send 替代 brpc 透传描述符）。
//
// 用法：./source <bind_ip> <port> [bytes=1048576]
// 例：  ./source 192.168.1.198 39321 1048576

#include <ucp/api/ucp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct PullDesc {
  std::uint64_t remote_addr;
  std::uint64_t length;
  std::uint64_t rkey_size;
  unsigned char rkey_buf[512];
};

constexpr std::uint64_t kTagDesc = 1;
constexpr std::uint64_t kTagDone = 2;

// listener conn_handler 回调里建好的 ep（单连接 demo，全局即可）。
std::atomic<ucp_ep_h> g_ep{nullptr};
std::atomic<bool> g_ep_ready{false};

// listener conn_handler：收到 puller 的连接请求 → 在回调里 ucp_ep_create
// (CONN_REQUEST)。回调在 worker progress 线程触发。
void ConnCb(ucp_conn_request_h req, void* arg) {
  auto worker = static_cast<ucp_worker_h>(arg);
  ucp_ep_params_t ep_params{};
  ep_params.field_mask =
      UCP_EP_PARAM_FIELD_CONN_REQUEST | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
  ep_params.conn_request = req;
  ep_params.err_mode = UCP_ERR_HANDLING_MODE_NONE;
  ucp_ep_h ep = nullptr;
  ucs_status_t st = ucp_ep_create(worker, &ep_params, &ep);
  if (st != UCS_OK) {
    std::fprintf(stderr, "source: conn cb ucp_ep_create failed: %s\n",
                 ucs_status_string(st));
    return;
  }
  g_ep.store(ep, std::memory_order_release);
  g_ep_ready.store(true, std::memory_order_release);
}

bool WaitOp(ucp_worker_h worker, void* req) {
  if (req == nullptr) return true;  // 内联完成。
  if (UCS_PTR_IS_ERR(req)) {
    std::fprintf(stderr, "source: op posted with error: %s\n",
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
    std::fprintf(stderr, "source: op completed with error: %s\n",
                 ucs_status_string(st));
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <bind_ip> <port> [bytes=1048576]\n", argv[0]);
    return 2;
  }
  const std::string bind_ip = argv[1];
  const int port = std::atoi(argv[2]);
  const std::uint64_t bytes =
      (argc >= 4) ? std::strtoull(argv[3], nullptr, 10) : (1ULL << 20);

  // ---- 1. context + worker ----
  ucp_params_t ctx_params{};
  ctx_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  ctx_params.features = UCP_FEATURE_RMA | UCP_FEATURE_TAG;
  ucp_config_t* config = nullptr;
  if (ucp_config_read(nullptr, nullptr, &config) != UCS_OK) {
    std::fprintf(stderr, "source: ucp_config_read failed\n");
    return 1;
  }
  ucp_context_h context = nullptr;
  ucs_status_t st = ucp_init(&ctx_params, config, &context);
  ucp_config_release(config);
  if (st != UCS_OK) {
    std::fprintf(stderr, "source: ucp_init failed: %s\n",
                 ucs_status_string(st));
    return 1;
  }

  ucp_worker_params_t wparams{};
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wparams.thread_mode = UCS_THREAD_MODE_SINGLE;
  ucp_worker_h worker = nullptr;
  st = ucp_worker_create(context, &wparams, &worker);
  if (st != UCS_OK) {
    std::fprintf(stderr, "source: ucp_worker_create failed: %s\n",
                 ucs_status_string(st));
    ucp_cleanup(context);
    return 1;
  }

  // ---- 2. listener_create(conn_handler 绑 bind_ip:port) ----
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
    std::fprintf(stderr, "source: invalid bind ip '%s'\n", bind_ip.c_str());
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  ucp_listener_params_t lparams{};
  lparams.field_mask =
      UCP_LISTENER_PARAM_FIELD_SOCK_ADDR | UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lparams.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);
  lparams.sockaddr.addrlen = sizeof(addr);
  lparams.conn_handler.cb = &ConnCb;
  lparams.conn_handler.arg = worker;
  ucp_listener_h listener = nullptr;
  st = ucp_listener_create(worker, &lparams, &listener);
  if (st != UCS_OK) {
    std::fprintf(stderr, "source: ucp_listener_create failed: %s\n",
                 ucs_status_string(st));
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  std::printf("source: listening on %s:%d (bytes=%llu)\n", bind_ip.c_str(), port,
              static_cast<unsigned long long>(bytes));

  // ---- 3. 分配 + 填充 host buffer（buf[i] = i & 0xFF）----
  unsigned char* buf =
      static_cast<unsigned char*>(std::malloc(static_cast<size_t>(bytes)));
  if (buf == nullptr) {
    std::fprintf(stderr, "source: malloc %llu bytes failed\n",
                 static_cast<unsigned long long>(bytes));
    ucp_listener_destroy(listener);
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  for (std::uint64_t i = 0; i < bytes; ++i) buf[i] = static_cast<unsigned char>(i & 0xFF);

  // ---- 4. mem_map 注册 host buffer ----
  ucp_mem_map_params_t mparams{};
  mparams.field_mask =
      UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  mparams.address = buf;
  mparams.length = static_cast<size_t>(bytes);
  ucp_mem_h memh = nullptr;
  st = ucp_mem_map(context, &mparams, &memh);
  if (st != UCS_OK) {
    std::fprintf(stderr, "source: ucp_mem_map failed: %s\n", ucs_status_string(st));
    std::free(buf);
    ucp_listener_destroy(listener);
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }

  // ---- 5. rkey_pack（导出供 puller unpack）----
  void* rkey_buf = nullptr;
  size_t rkey_size = 0;
  st = ucp_rkey_pack(context, memh, &rkey_buf, &rkey_size);
  if (st != UCS_OK || rkey_buf == nullptr) {
    std::fprintf(stderr, "source: ucp_rkey_pack failed: %s\n", ucs_status_string(st));
    ucp_mem_unmap(context, memh);
    std::free(buf);
    ucp_listener_destroy(listener);
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  std::printf("source: mem_map'd buf=%p rkey_size=%zu\n", buf, rkey_size);

  // ---- 6. progress 等到 puller dial 进来建好 ep ----
  while (!g_ep_ready.load(std::memory_order_acquire)) {
    ucp_worker_progress(worker);
  }
  ucp_ep_h ep = g_ep.load(std::memory_order_acquire);
  std::printf("source: ep ready\n");
  // 打印 ep 实际选用的传输（观察是否走 rc_mlx5 RDMA fabric）。
  ucp_ep_print_info(ep, stderr);

  // ---- 7. tag_send 把 PullDesc 发给 puller ----
  PullDesc desc{};
  desc.remote_addr = reinterpret_cast<std::uint64_t>(buf);
  desc.length = bytes;
  desc.rkey_size = static_cast<std::uint64_t>(rkey_size);
  if (rkey_size > sizeof(desc.rkey_buf)) {
    std::fprintf(stderr, "source: rkey too large (%zu)\n", rkey_size);
    ucp_rkey_buffer_release(rkey_buf);
    ucp_mem_unmap(context, memh);
    std::free(buf);
    ucp_listener_destroy(listener);
    ucp_worker_destroy(worker);
    ucp_cleanup(context);
    return 1;
  }
  std::memcpy(desc.rkey_buf, rkey_buf, rkey_size);
  ucp_request_param_t send_param{};
  void* send_req =
      ucp_tag_send_nbx(ep, &desc, sizeof(desc), kTagDesc, &send_param);
  if (!WaitOp(worker, send_req)) goto cleanup;

  // ---- 8. tag_recv 等 puller 的 "done" ----
  {
    std::uint64_t done = 0;
    ucp_request_param_t recv_param{};
    void* recv_req = ucp_tag_recv_nbx(worker, &done, sizeof(done), kTagDone,
                                      0xFFFFFFFFFFFFFFFFULL, &recv_param);
    WaitOp(worker, recv_req);
    if (done == 1) {
      std::printf("source: done received, puller verified bytes=%llu\n",
                  static_cast<unsigned long long>(bytes));
    } else {
      std::fprintf(stderr, "source: unexpected done=%llu\n",
                   static_cast<unsigned long long>(done));
    }
  }

cleanup:
  ucp_rkey_buffer_release(rkey_buf);
  ucp_mem_unmap(context, memh);
  std::free(buf);
  {
    ucp_request_param_t close_param{};
    void* close_req = ucp_ep_close_nbx(ep, &close_param);
    if (close_req != nullptr && !UCS_PTR_IS_ERR(close_req)) {
      while (ucp_request_check_status(close_req) == UCS_INPROGRESS) {
        ucp_worker_progress(worker);
      }
      ucp_request_free(close_req);
    }
  }
  ucp_listener_destroy(listener);
  ucp_worker_destroy(worker);
  ucp_cleanup(context);
  return 0;
}
