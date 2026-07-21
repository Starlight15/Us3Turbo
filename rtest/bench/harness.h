// rtest/bench/harness.h — 共享 bench 基础设施（通路无关，header-only，无 CUDA 依赖）。
//
// 提供 bench 文件间复用的类型别名、统计函数、参数基类和结果结构体。
// 不包含任何通路特定逻辑（CUDA/ibverbs）。具体 bench 文件通过派生 BaseArgs
// 添加通路特定字段，通过 RunOneRound 自定义测量逻辑。
//
// 用法：
//   1. 派生 Args : rtest::bench::BaseArgs，添加通路特定参数
//   2. 定义 RunOneRound() 返回 RoundResult
//   3. main() 中 ParseArgs → 初始化 client → 跑 warmup+reps → ComputeStats → PrintReport
//
// 命名空间：rtest::bench

#pragma once

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "rtest/common.h"

namespace rtest::bench {

// ---- 类型别名 ----
using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

// ---- 统计工具 ----

inline double Median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

inline double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t idx =
      std::min(v.size() - 1, static_cast<std::size_t>(p / 100.0 * (v.size() - 1)));
  return v[idx];
}

inline double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double s = 0.0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

inline double Min(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  return *std::min_element(v.begin(), v.end());
}

inline double Max(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  return *std::max_element(v.begin(), v.end());
}

// ---- 参数基类 ----
// 所有 bench 共用的参数字段。通路特定字段（size/count/part_size 等）
// 通过派生添加。

struct BaseArgs {
  std::string proxy{"192.168.1.198:9100"};
  std::uint64_t total{64ULL * 1024 * 1024};
  std::uint32_t reps{5};
  std::uint32_t warmup{0};
  std::uint32_t concurrency{1};
  std::string bucket{"test-bucket"};
  std::string key_prefix{"bench"};
  bool verify_crc32c{false};
  bool trace{false};
  bool csv{false};
};

// ---- 单轮结果 ----
// 每轮 bench 通用结果结构。通路特定 bench 可在 RunOneRound 中填充。

struct RoundResult {
  double setup_ms{0};         // 准备阶段（如 CreateMultipartUpload）
  double data_plane_ms{0};    // 纯数据传输（PUT / UploadPart）
  double control_plane_ms{0}; // 控制面操作（如 CompleteMultipartUpload）
  double total_ms{0};         // 整轮端到端耗时
  std::uint64_t bytes{0};     // 本轮传输字节数
  bool ok{false};
  std::string error;
};

// ---- 统计聚合 ----

struct AggregateStats {
  std::size_t n{0};
  double min_ms{0};
  double p50_ms{0};
  double p95_ms{0};
  double max_ms{0};
  double mean_ms{0};
  double throughput_mibps{0};
};

// 从 RoundResult 向量计算总耗时维度的聚合统计。
inline AggregateStats ComputeStats(const std::vector<RoundResult>& rounds, double wall_ms) {
  std::vector<double> total_ms;
  total_ms.reserve(rounds.size());
  std::uint64_t total_bytes = 0;
  for (const auto& r : rounds) {
    if (!r.ok) continue;
    total_ms.push_back(r.total_ms);
    total_bytes += r.bytes;
  }
  AggregateStats s;
  s.n = total_ms.size();
  if (s.n > 0) {
    s.min_ms = Min(total_ms);
    s.p50_ms = Median(total_ms);
    s.p95_ms = Percentile(total_ms, 95);
    s.max_ms = Max(total_ms);
    s.mean_ms = Mean(total_ms);
    const double wall_s = wall_ms / 1000.0;
    s.throughput_mibps =
        (wall_s > 0.0) ? static_cast<double>(total_bytes) / wall_s / (1024.0 * 1024.0) : 0.0;
  }
  return s;
}

// 从 RoundResult 的某个阶段字段提取统计。
inline AggregateStats PhaseStats(const std::vector<RoundResult>& rounds,
                                 double (RoundResult::*field)) {
  std::vector<double> vals;
  vals.reserve(rounds.size());
  for (const auto& r : rounds) {
    if (!r.ok) continue;
    vals.push_back(r.*field);
  }
  AggregateStats s;
  s.n = vals.size();
  if (s.n > 0) {
    s.min_ms = Min(vals);
    s.p50_ms = Median(vals);
    s.p95_ms = Percentile(vals, 95);
    s.max_ms = Max(vals);
    s.mean_ms = Mean(vals);
  }
  return s;
}

// ---- 打印 ----

inline void PrintReport(std::string_view bench_name, const std::vector<RoundResult>& rounds,
                        double wall_ms) {
  std::uint32_t ok = 0, fail = 0;
  std::uint64_t total_bytes = 0;
  for (const auto& r : rounds) {
    if (r.ok) {
      ++ok;
      total_bytes += r.bytes;
    } else {
      ++fail;
    }
  }

  const auto stats = ComputeStats(rounds, wall_ms);
  const auto setup_st = PhaseStats(rounds, &RoundResult::setup_ms);
  const auto data_st = PhaseStats(rounds, &RoundResult::data_plane_ms);
  const auto ctrl_st = PhaseStats(rounds, &RoundResult::control_plane_ms);

  std::cout << "=== results (" << bench_name << ") ===\n"
            << "  ok          : " << ok << "\n"
            << "  fail        : " << fail << "\n"
            << "  bytes       : " << rtest::HumanBytes(total_bytes) << "\n"
            << "  wall time   : " << wall_ms << " ms\n"
            << "  throughput  : " << stats.throughput_mibps << " MiB/s\n";

  if (stats.n > 0) {
    // 总耗时分布
    std::cout << "  total(ms)   : avg=" << stats.mean_ms << "  p50=" << stats.p50_ms
              << "  p95=" << stats.p95_ms << "  min=" << stats.min_ms
              << "  max=" << stats.max_ms << "\n";

    // 阶段统计（仅当有数据时打印）
    auto print_phase = [](const char* label, const AggregateStats& s) {
      if (s.n == 0) return;
      std::cout << "  " << label << "(ms) : avg=" << s.mean_ms << "  p50=" << s.p50_ms
                << "  p95=" << s.p95_ms << "  min=" << s.min_ms << "  max=" << s.max_ms << "\n";
    };
    print_phase("setup      ", setup_st);
    print_phase("data-plane ", data_st);
    print_phase("ctrl-plane ", ctrl_st);
  }
  std::cout.flush();
}

// ---- CSV 输出 ----

inline void WriteCsv(std::string_view path_name, const BaseArgs& a,
                     const std::vector<RoundResult>& rounds, std::uint32_t num_parts = 0) {
  std::cout << "path,total_bytes,parts,concurrency,rep,"
               "setup_ms,data_plane_ms,control_plane_ms,total_ms,ok\n";
  std::uint32_t rep = 0;
  for (const auto& r : rounds) {
    std::cout << path_name << "," << a.total << "," << num_parts << "," << a.concurrency << ","
              << rep++ << "," << r.setup_ms << "," << r.data_plane_ms << "," << r.control_plane_ms
              << "," << r.total_ms << "," << (r.ok ? 1 : 0) << "\n";
  }
}

// ---- barrier 同步 ----
// barrier 的 completion functor：最后一个到达的线程记录统一起跑时刻。

struct StartSetter {
  std::atomic<clk::time_point>* start;
  void operator()() const noexcept { start->store(clk::now(), std::memory_order_relaxed); }
};

}  // namespace rtest::bench
