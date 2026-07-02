#pragma once

// put_trace.h — 链路无关通用工具(MakeRequestId / TraceLatency / LatencyStage)。
// 干净共享头,不含 cuObj/ucp 依赖。

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

// 每次(含每次重试)生成新 request_id,用于跨端日志关联。
[[nodiscard]] inline std::string MakeRequestId() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::random_device{}()) ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())};
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", rng());
  return std::string("req-") + buf;
}

// 性能追踪(options.latency_trace 开启):相邻阶段耗时 + 首→末总耗时。
struct LatencyStage {
  std::string_view    name;
  clk::time_point     timestamp;
};

// stage1/stage2/... 为相邻阶段耗时,total 为首→末总耗时。
inline void TraceLatency(const std::string& request_id,
                          std::string_view operation_name,
                          std::span<const LatencyStage> stages,
                          std::size_t bytes) {
  const auto ms = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

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
