#pragma once

// snowflake.h — proxy 侧 trace_id 生成器(与 s3proxy 的 snowflake Tracer 对齐)。
// 64-bit: 1b 符号(0) | 41b 毫秒时间戳 | 10b node(0~1023) | 12b 序列(同毫秒内)。
// proxy 进程级全局唯一;node 由 gflag --snowflake_node_id 给(多实例须不同)。
// 生成的 trace_id 写进每条下行(dbgate session_no / ufile-ac sessionId /
// minit.fileid / backend [rdma-*] 日志),并在 RPC 响应里回 client。

#include <chrono>
#include <cstdint>
#include <mutex>

namespace us3_turbo::proxy {

class Snowflake {
 public:
  static Snowflake& Instance() {
    static Snowflake s;
    return s;
  }

  void SetNode(std::uint64_t node) { node_ = node & kNodeMask; }

  // 生成下一个 trace_id。同毫秒内序列自增;序列溢出则自旋到下一毫秒(保证不重复)。
  std::uint64_t Next() {
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t now = NowMs();
    if (now == last_ms_) {
      seq_ = (seq_ + 1) & kSeqMask;
      if (seq_ == 0) {  // 同毫秒内超过 4096 个,等下一毫秒
        while ((now = NowMs()) == last_ms_) {
          // spin
        }
      }
    } else {
      seq_ = 0;
    }
    last_ms_ = now;
    return ((now - kEpoch) << kSeqShift) | (node_ << kSeqBits) | seq_;
  }

 private:
  Snowflake() = default;

  static std::uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static constexpr std::uint64_t kEpoch = 1700000000000ULL;  // 2023-11-14 UTC
  static constexpr int kSeqBits = 12;
  static constexpr int kNodeBits = 10;
  static constexpr std::uint64_t kSeqMask = (1ULL << kSeqBits) - 1;
  static constexpr std::uint64_t kNodeMask = (1ULL << kNodeBits) - 1;
  static constexpr int kSeqShift = kSeqBits;

  std::mutex mu_;
  std::uint64_t last_ms_{0};
  std::uint64_t seq_{0};
  std::uint64_t node_{1};
};

inline std::uint64_t GenTraceId() {
  return Snowflake::Instance().Next();
}

// 当前 RPC 的 trace_id(brpc 同步 handler:一个 worker 线程同时只服务一个 RPC,
// 故 handler 入口 set 后,整个 RPC 内(含 multipart/single_put/get_object →
// dbgate/ufile-ac 调用,均同线程)读到的就是本 RPC 的 trace_id)。
// 叶层(dbgate ExecuteMgo 的 session_no / ufile_ac_client Encode 的 session_id /
// InsertMinit 的 fileid)直接取此值,免去跨 IUploadIndex/DBGateClient 全方法
// 显式参数级联。功能上与 s3proxy 显式传 *Tracer 指针等价。
inline std::uint64_t& CurrentTraceId() {
  thread_local std::uint64_t tid{0};
  return tid;
}

}  // namespace us3_turbo::proxy
