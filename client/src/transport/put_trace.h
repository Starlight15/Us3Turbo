#pragma once

// put_trace.h — 链路无关的通用工具(下沉自 client.cpp)。
//
// 只有 MakeRequestId / TraceLatency / LatencyStage 三个通用件共享,两条链路
// 都 include。链路特有逻辑(GDS token / UCX 描述符 / 各自 CRC)不下沉——
// 仍留在各自的 *_put_channel.cpp 里(见 review/client_refactor_prompt.md)。
//
// 本头是「干净」共享头,不含 cuObj / ucp 依赖。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::client {

namespace detail {

using clk = std::chrono::steady_clock;

// 每次（含每次重试）生成新 request_id，用于跨端日志关联。
// 原 client.cpp 匿名 namespace 内实现,逐字搬运。
[[nodiscard]] inline std::string MakeRequestId() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::random_device{}()) ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())};
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", rng());
  return std::string("req-") + buf;
}

// ---------------------------------------------------------------------------
//  性能追踪（可选，options.latency_trace 开启）
//  通用：按相邻阶段时间差输出毫秒（3 位小数），总时间 = 首→末时间戳差。
// ---------------------------------------------------------------------------

struct LatencyStage {
  std::string_view    name;
  clk::time_point     timestamp;
};

// 通用的性能追踪函数：stage1/stage2/... 为相邻阶段耗时，total 为首→末总耗时。
// 原 client.cpp 匿名 namespace 内实现,逐字搬运。
inline void TraceLatency(const std::string& request_id,
                          std::string_view operation_name,
                          std::span<const LatencyStage> stages,
                          std::size_t bytes) {
  const auto ms = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  // 拼接各相邻阶段 "name={:.3f}ms "，原内联格式为 "token={:.3f}ms put={:.3f}ms total=..."。
  std::string parts;
  for (std::size_t i = 1; i < stages.size(); ++i) {
    parts += fmt::format("{}={:.3f}ms ", stages[i].name, ms(stages[i - 1].timestamp,
                                                            stages[i].timestamp));
  }
  const double total =
      stages.size() >= 2 ? ms(stages.front().timestamp, stages.back().timestamp) : 0.0;

  spdlog::info("{} trace (req={}): {}total={:.3f}ms bytes={}",
               operation_name, request_id, parts, total, bytes);
}

}  // namespace detail

}  // namespace us3_turbo::client
