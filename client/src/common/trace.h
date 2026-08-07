#pragma once

// trace.h — 链路无关通用工具(MakeReqId / TraceLatency / LatencyStage)。

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

/** @brief 生成新 req_id,client 侧本地日志句柄(不发往 proxy,非跨层 trace_id;
 * 跨层关联用 RPC 响应里的 proxy snowflake trace_id,经 res.trace_id 取)。*/
[[nodiscard]] inline std::string MakeReqId() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::random_device{}()) ^
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", rng());
  return std::string("req-") + buf;
}

/** @brief 性能追踪阶段名 + 时间戳。*/
struct LatencyStage {
  std::string_view name;
  clk::time_point timestamp;
};

/** @brief 打印相邻阶段耗时 + 首→末总耗时(latency_trace 开启时调用)。
 *
 * 输出统一规范:[perf/client] req=<id> op=<op> <stage>_us=<n> ... total_us=<n> bytes=<n>
 * µs 粒度、key=val、与 proxy [perf/proxy] / backend [rdma-*] 同风格,可 grep+python 聚合。
 * stages[i].name 标注的是"到该阶段为止"的相邻段耗时(stages[i-1]→stages[i])。*/
inline void TraceLatency(std::uint64_t trace_id, std::string_view operation_name,
                         std::span<const LatencyStage> stages, std::size_t bytes) {
  const auto us = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
  };

  std::string parts;
  for (std::size_t i = 1; i < stages.size(); ++i) {
    parts += fmt::format("{}_us={} ", stages[i].name,
                         us(stages[i - 1].timestamp, stages[i].timestamp));
  }
  const auto total_us = stages.size() >= 2
                            ? us(stages.front().timestamp, stages.back().timestamp)
                            : std::int64_t{0};

  spdlog::info("[perf/client] req={} op={} {}total_us={} bytes={}", trace_id,
               operation_name, parts, total_us, bytes);
}

}  // namespace detail

}  // namespace us3_turbo::client
