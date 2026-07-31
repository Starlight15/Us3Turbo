// harness.h — bench 共享基础设施（通路无关，header-only）。
//
// 提供: 统计工具、公共类型 (BaseArgs/RoundResult/WorkerStats)、
// 结果打印 (PrintReport/WriteCsv)、barrier 同步。

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

// 字符串 → uint64_t（各 bench 文件原先各有自己的 ParseUint 副本）。
inline bool ParseUint(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = v;
  return true;
}

// ---- 公共类型 ----

// 基准测试共用参数。通路特定字段（size/count/part_size 等）由各 bench 在局部
// struct 中派生添加。
struct BaseArgs {
  std::string proxy{rtest::kDefaultProxyEndpoint};
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

// 单轮结果。各 bench 的 RunOneRound / do_put / do_get 填充此结构。
struct RoundResult {
  double setup_ms{0};         // 准备阶段（如 CreateMultipartUpload）
  double data_plane_ms{0};    // 纯数据传输（PUT / UploadPart）
  double control_plane_ms{0}; // 控制面操作（如 CompleteMultipartUpload）
  double total_ms{0};         // 整轮端到端耗时
  std::uint64_t bytes{0};     // 本轮传输字节数
  bool ok{false};
  std::string error;
};

// 单个 worker 的统计——各 PUT/GET bench worker 都在本地聚合。
struct WorkerStats {
  std::vector<RoundResult> rounds;
  std::uint32_t ok{0};
  std::uint32_t fail{0};
  std::uint64_t bytes{0};
  clk::time_point end{};
  bool ready{false};
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

// ---- 结果打印 ----

// multipart bench 结果报告。
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
    std::cout << "  total(ms)   : avg=" << stats.mean_ms << "  p50=" << stats.p50_ms
              << "  p95=" << stats.p95_ms << "  min=" << stats.min_ms
              << "  max=" << stats.max_ms << "\n";

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

// PUT/GET bench 结果报告 (单阶段计时，无 setup/ctrl 分阶段)。
inline void PrintPutGetResults(std::string_view path, std::string_view op,
                                const std::vector<WorkerStats>& stats_vec,
                                std::uint32_t concurrency,
                                clk::time_point t_start) {
  // 聚合
  std::uint32_t ok = 0, fail = 0;
  std::uint64_t bytes = 0;
  std::vector<RoundResult> all;
  clk::time_point end_max{};
  bool any_ready = false;
  for (std::size_t w = 0; w < stats_vec.size(); ++w) {
    if (!stats_vec[w].ready) {
      std::cerr << "[worker " << w << "] not ready (buffer alloc/register failed)\n";
      continue;
    }
    any_ready = true;
    ok += stats_vec[w].ok;
    fail += stats_vec[w].fail;
    bytes += stats_vec[w].bytes;
    all.insert(all.end(), stats_vec[w].rounds.begin(), stats_vec[w].rounds.end());
    if (end_max.time_since_epoch().count() == 0)
      end_max = stats_vec[w].end;
    else
      end_max = std::max(end_max, stats_vec[w].end);
  }

  if (!any_ready) {
    std::cerr << "no worker was ready — aborting\n";
    return;
  }

  const double wall_ms = ms_double(end_max - t_start).count();
  const double wall_s = wall_ms / 1000.0;
  const double mibs = (wall_s > 0.0) ? static_cast<double>(bytes) / wall_s / (1024.0 * 1024.0) : 0.0;
  const double ops = (wall_s > 0.0) ? static_cast<double>(ok) / wall_s : 0.0;

  // 时延分布
  std::vector<double> lat;
  lat.reserve(all.size());
  for (const auto& r : all)
    if (r.ok) lat.push_back(r.data_plane_ms);
  std::sort(lat.begin(), lat.end());

  std::cout << "\n=== results ===\n"
            << "  ok           : " << ok << "\n"
            << "  fail         : " << fail << "\n"
            << "  bytes        : " << rtest::HumanBytes(bytes) << " (" << bytes << ")\n"
            << "  wall time    : " << wall_ms << " ms\n"
            << "  throughput   : " << mibs << " MiB/s  (" << ops << " ops/s)\n";

  if (!lat.empty()) {
    std::cout << "  latency (ms) : avg=" << rtest::bench::Mean(lat)
              << "  min=" << lat.front()
              << "  p50=" << rtest::bench::Percentile(lat, 50.0)
              << "  p95=" << rtest::bench::Percentile(lat, 95.0)
              << "  p99=" << rtest::bench::Percentile(lat, 99.0)
              << "  max=" << lat.back() << "\n";
  }
  std::cout.flush();
}

// multipart bench CSV 输出。
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
// barrier completion functor：最后一个到达的线程记录统一起跑时刻。

struct StartSetter {
  std::atomic<clk::time_point>* start;
  void operator()() const noexcept { start->store(clk::now(), std::memory_order_relaxed); }
};

}  // namespace rtest::bench
