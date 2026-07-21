// gds_put_bench.cpp — GDS 单步 PUT 性能基准（rtest/bench/gds）。
//
// 测量维度：
//   1. 吞吐 —— 多 worker 并发 PUT 聚合吞吐（MiB/s, ops/s）
//   2. 时延分布 —— min/p50/p95/p99/max per-PUT latency
//
// 用法：
//   us3_turbo_bench_gds_put \
//     --proxy 192.168.1.198:9100 \
//     --size 100M --count 100 --concurrency 8
//
// 模型：
//   进程内共享一个 Client（PutObject 为 const，brpc channel 与
//   GdsMemoryManager 单例均线程安全）；每个 worker 线程拥有独立的 device
//   buffer，从共享原子计数器领取对象序号并发上传。
//   首次 PutObject 时在 GdsPutChannel 内部懒注册，无需显式 Register。

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/bench/harness.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {

using rtest::bench::clk;
using rtest::bench::ms_double;
using rtest::bench::RoundResult;
using rtest::bench::StartSetter;

constexpr char kPathName[] = "gds";

// ---- 参数（通路特定字段通过派生添加） ----

struct Args : rtest::bench::BaseArgs {
  std::uint64_t size{100ULL * 1024 * 1024};  // 单个对象大小
  std::uint64_t count{10};                    // 总对象数
};

bool ParseUint(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = v;
  return true;
}

bool ParseArgs(int argc, char** argv, Args& a) {
  auto need = [&](int& i, std::string_view& val) -> bool {
    if (i + 1 >= argc) {
      std::cerr << "missing value for " << argv[i] << "\n";
      return false;
    }
    val = argv[++i];
    return true;
  };
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    std::string_view val;
    if (arg == "--proxy") {
      if (!need(i, val)) return false;
      a.proxy = std::string(val);
    } else if (arg == "--size") {
      if (!need(i, val) || !rtest::ParseSize(val, a.size)) {
        std::cerr << "bad --size\n";
        return false;
      }
    } else if (arg == "--count") {
      if (!need(i, val) || !ParseUint(val, a.count)) {
        std::cerr << "bad --count\n";
        return false;
      }
    } else if (arg == "--concurrency") {
      std::uint64_t v;
      if (!need(i, val) || !ParseUint(val, v)) {
        std::cerr << "bad --concurrency\n";
        return false;
      }
      a.concurrency = static_cast<std::uint32_t>(v);
    } else if (arg == "--warmup") {
      std::uint64_t v;
      if (!need(i, val) || !ParseUint(val, v)) {
        std::cerr << "bad --warmup\n";
        return false;
      }
      a.warmup = static_cast<std::uint32_t>(v);
    } else if (arg == "--bucket") {
      if (!need(i, val)) return false;
      a.bucket = std::string(val);
    } else if (arg == "--key-prefix") {
      if (!need(i, val)) return false;
      a.key_prefix = std::string(val);
    } else if (arg == "--verify-crc32c") {
      a.verify_crc32c = true;
    } else if (arg == "--trace") {
      a.trace = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: us3_turbo_bench_" << kPathName << "_put [options]\n"
                << "  --proxy HOST:PORT        proxy endpoint (default 192.168.1.198:9100)\n"
                << "  --size N[K|M|G]          object size (default 100M)\n"
                << "  --count N                number of objects (default 10)\n"
                << "  --concurrency N          worker threads (default 1)\n"
                << "  --warmup N               warmup ops, not counted (default 0)\n"
                << "  --bucket NAME            bucket (default bench)\n"
                << "  --key-prefix STR         key prefix (default bench)\n"
                << "  --verify-crc32c          enable client-side CRC32C verification\n"
                << "  --trace                  log per-PUT stage latency\n";
      return false;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  if (a.size == 0 || a.count == 0 || a.concurrency == 0) {
    std::cerr << "size/count/concurrency must be > 0\n";
    return false;
  }
  return true;
}

// ---- worker ----

struct WorkerStats {
  std::vector<RoundResult> rounds;
  std::uint64_t ok{0};
  std::uint64_t fail{0};
  std::uint64_t bytes{0};
  clk::time_point end{};
  bool ready{false};
};

void Worker(std::size_t wid, const Args& a, us3_turbo::client::Client& client,
            const std::byte* host_pattern, std::atomic<std::uint64_t>& next, std::uint64_t total,
            std::barrier<StartSetter>& sync, std::atomic<clk::time_point>& start,
            WorkerStats& stats) {
  using namespace us3_turbo::client;

  // 1) 分配并填充 device buffer（每个 worker 独立）。
  void* dev = nullptr;
  cudaError_t e = cudaMalloc(&dev, a.size);
  if (e != cudaSuccess) {
    std::cerr << "[worker " << wid << "] cudaMalloc(" << rtest::HumanBytes(a.size)
              << ") failed: " << cudaGetErrorString(e) << "\n";
    return;
  }
  e = cudaMemcpy(dev, host_pattern, a.size, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    std::cerr << "[worker " << wid << "] cudaMemcpy failed: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return;
  }
  stats.ready = true;

  ConstBufferView buf{.data = dev, .size = a.size};

  auto do_put = [&](const std::string& key) -> bool {
    ClientProxyPutRequest req;
    req.bucket = a.bucket;
    req.key = key;
    req.object_size = a.size;
    req.path = PutDataPath::kGds;
    ClientProxyPutResponse out;
    auto t0 = clk::now();
    bool ok = client.PutObject(req, buf, out);
    auto t1 = clk::now();
    if (ok) {
      stats.rounds.push_back(RoundResult{.data_plane_ms = ms_double(t1 - t0).count(),
                                          .total_ms = ms_double(t1 - t0).count(),
                                          .bytes = a.size,
                                          .ok = true});
      ++stats.ok;
      stats.bytes += a.size;
    } else {
      stats.rounds.push_back(RoundResult{.ok = false});
      ++stats.fail;
    }
    return ok;
  };

  // 2) warmup（不计入统计，key 与正式对象隔离）。
  for (std::uint64_t i = 0; i < a.warmup; ++i) {
    do_put(a.key_prefix + "-warmup-" + std::to_string(wid) + "-" + std::to_string(i));
  }

  // 3) 屏障对齐后开始计时。
  sync.arrive_and_wait();

  // 4) 正式测量：从原子计数器领取序号直至 total。
  std::uint64_t idx;
  while ((idx = next.fetch_add(1, std::memory_order_relaxed)) < total) {
    do_put(a.key_prefix + "-" + std::to_string(idx));
  }
  stats.end = clk::now();

  cudaFree(dev);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  Args a;
  a.warmup = 0;  // put bench 默认 0（与 BaseArgs::warmup=1 不同）
  if (!ParseArgs(argc, argv, a)) return 1;

  std::cout << "=== " << kPathName << " PUT bench ===\n"
            << "  proxy       : " << a.proxy << "\n"
            << "  object size : " << rtest::HumanBytes(a.size) << "\n"
            << "  count       : " << a.count << "\n"
            << "  concurrency : " << a.concurrency << "\n"
            << "  warmup      : " << a.warmup << "\n"
            << "  bucket      : " << a.bucket << "\n"
            << "  key-prefix  : " << a.key_prefix << "\n"
            << "  verify-crc  : " << (a.verify_crc32c ? "on" : "off") << "\n"
            << std::endl;

  // 共享 host pattern（只读，各 worker 并发拷贝）。
  std::vector<std::byte> host(a.size);
  rtest::FillHostPattern(host);

  ClientOptions opts;
  opts.endpoint = a.proxy;
  opts.verify_crc32c = a.verify_crc32c;
  opts.latency_trace = a.trace;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  const std::size_t nworkers = static_cast<std::size_t>(a.concurrency);
  std::atomic<std::uint64_t> next{0};
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});

  std::vector<WorkerStats> stats(nworkers);
  std::vector<std::thread> threads;
  threads.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w) {
    threads.emplace_back(Worker, w, std::ref(a), std::ref(client), host.data(), std::ref(next),
                         a.count, std::ref(sync), std::ref(start), std::ref(stats[w]));
  }
  for (auto& t : threads) t.join();

  // ---- 汇总 ----
  std::uint64_t ok = 0, fail = 0, bytes = 0;
  std::vector<RoundResult> all;
  clk::time_point end_max{};
  bool any_ready = false;
  for (std::size_t w = 0; w < nworkers; ++w) {
    if (!stats[w].ready) {
      std::cerr << "[worker " << w << "] not ready (buffer alloc/register failed)\n";
      continue;
    }
    any_ready = true;
    ok += stats[w].ok;
    fail += stats[w].fail;
    bytes += stats[w].bytes;
    all.insert(all.end(), stats[w].rounds.begin(), stats[w].rounds.end());
    if (end_max.time_since_epoch().count() == 0) {
      end_max = stats[w].end;
    } else {
      end_max = std::max(end_max, stats[w].end);
    }
  }

  if (!any_ready) {
    std::cerr << "no worker was ready — aborting\n";
    client.Shutdown();
    return 1;
  }

  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  const double wall_ms = ms_double(end_max - t_start).count();
  const double wall_s = wall_ms / 1000.0;
  const double throughput_mbs =
      (wall_s > 0.0) ? static_cast<double>(bytes) / wall_s / (1024.0 * 1024.0) : 0.0;
  const double ops_per_sec = (wall_s > 0.0) ? static_cast<double>(ok) / wall_s : 0.0;

  // 提取时延样本。
  std::vector<double> lat;
  lat.reserve(all.size());
  for (const auto& r : all) {
    if (r.ok) lat.push_back(r.data_plane_ms);
  }
  std::sort(lat.begin(), lat.end());

  std::cout << "\n=== results ===\n"
            << "  ok           : " << ok << "\n"
            << "  fail         : " << fail << "\n"
            << "  bytes        : " << rtest::HumanBytes(bytes) << " (" << bytes << ")\n"
            << "  wall time    : " << wall_ms << " ms\n"
            << "  throughput   : " << throughput_mbs << " MiB/s  (" << ops_per_sec << " ops/s)\n";
  if (!lat.empty()) {
    std::cout << "  latency (ms) : avg=" << rtest::bench::Mean(lat)
              << "  min=" << lat.front()
              << "  p50=" << rtest::bench::Percentile(lat, 50.0)
              << "  p95=" << rtest::bench::Percentile(lat, 95.0)
              << "  p99=" << rtest::bench::Percentile(lat, 99.0)
              << "  max=" << lat.back() << "\n";
  }
  std::cout.flush();

  client.Shutdown();
  return fail == 0 ? 0 : 2;
}
