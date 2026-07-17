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

/** @brief 生成新 req_id,用于跨端日志关联(每次重试都新生成)。*/
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

/** @brief 打印相邻阶段耗时 + 首→末总耗时(latency_trace 开启时调用)。*/
inline void TraceLatency(const std::string& req_id, std::string_view operation_name,
                         std::span<const LatencyStage> stages, std::size_t bytes) {
  const auto ms = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  std::string parts;
  for (std::size_t i = 1; i < stages.size(); ++i) {
    parts += fmt::format("{}={:.3f}ms ", stages[i].name,
                         ms(stages[i - 1].timestamp, stages[i].timestamp));
  }
  const double total =
      stages.size() >= 2 ? ms(stages.front().timestamp, stages.back().timestamp) : 0.0;

  spdlog::info("{} trace (req={}): {}total={:.3f}ms bytes={}", operation_name, req_id, parts, total,
               bytes);
}

}  // namespace detail

}  // namespace us3_turbo::client
