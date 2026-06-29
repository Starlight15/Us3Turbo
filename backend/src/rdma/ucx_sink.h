#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <ucp/api/ucp.h>

namespace us3_turbo::backend::rdma {

/**
 * @brief UCX RDMA PUT 数据面接收结果（与 gds 链路的 DiscardOutcome 平行、独立）。
 *
 * ok=false 时 error 带失败原因；ok=true 时 bytes_transferred 为 ucp_get_nbx
 * 实际拉入本地 buffer 的字节数，crc32c 为前 transferred 字节的 CRC32C
 * （供端到端校验），字节随后丢弃。
 */
struct DiscardOutcome {
  bool          ok{false};
  std::string   error;
  std::uint64_t bytes_transferred{0};
  std::uint32_t crc32c{0};
};

/**
 * @brief 拥有 UCX context + worker，复刻 gds BackendGdsSink 的收字节流程，
 *        但拉到的字节直接丢弃（不写盘），且 backend 主动 dial client 建 ep。
 *
 * 与 BackendGdsSink 的关键差异：GDS 用 cuObjServer DC（connectionless，
 * token 自描述），backend 拿 token 即拉；UCX RMA 是连接式，ucp_get_nbx 必须
 * 在已建 ep 上发起，故本 sink 在 ReceiveAndDiscard 内按 client_ucx_addr
 * 主动 ucp_ep_create(SOCK_ADDR) 建 ep，再 unpack rkey、get_nbx 拉。ep 用完
 * 即关（第一版不缓存，简单优先）。
 *
 * 启动顺序：Start() 建 context + worker；必须在 brpc 接受请求前完成，
 * 故 ReceiveAndDiscard 无需加锁访问 context_/worker_（服务期间恒定）。
 * 单 worker、UCS_THREAD_MODE_SINGLE：第一版不追求并发，每次拉取串行
 * progress。brpc 多 bthread 会并发调 ReceiveAndDiscard，故内部用一把锁
 * 串行化 worker 访问（单 worker 非线程安全）。
 */
class UcxSink {
 public:
  /**
   * @param compute_crc32c 为 false 时跳过对收到字节的 CRC32C 扫描
   *        (outcome.crc32c 恒为 0),用于关闭校验做纯吞吐压测。
   */
  explicit UcxSink(bool compute_crc32c = true);
  ~UcxSink();

  UcxSink(const UcxSink&) = delete;
  UcxSink& operator=(const UcxSink&) = delete;

  /** 起 UCX context + worker；失败返回 false。 */
  bool Start();

  void Stop();

  [[nodiscard]] bool available() const;

  /**
   * @brief 主动 dial client_ucx_addr 建 ep，unpack rkey，用 ucp_get_nbx 拉
   *        length 字节进本地 malloc buffer 后丢弃。
   *
   * length==0 视为成功空传输。
   * client_ucx_addr 形如 "ip:port"。remote_addr 为 client 侧 host buffer 虚拟地址。
   * rkey 为 ucp_rkey_pack 导出的 packed rkey。
   */
  DiscardOutcome ReceiveAndDiscard(const std::string& object_id,
                                   const std::string& client_ucx_addr,
                                   std::uint64_t remote_addr,
                                   const std::string& rkey,
                                   std::uint64_t length);

 private:
  bool compute_crc32c_;
  ucp_context_h context_{nullptr};
  ucp_worker_h worker_{nullptr};
  std::mutex worker_mu_;  // 串行化单 worker 访问
};

}  // namespace us3_turbo::backend::rdma
