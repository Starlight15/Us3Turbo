// rdma_put_bench.cpp — RDMA 单步 PUT 性能基准。
//
// 测量: 多 worker 并发吞吐 (MiB/s, ops/s) 和 per-PUT 时延分布。host 内存，无 CUDA 依赖。

#include <atomic>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/bench/harness.h"
#include "us3_turbo/client/client.h"

using rtest::bench::clk;
using rtest::bench::ms_double;
using rtest::bench::RoundResult;
using rtest::bench::StartSetter;

// ---- 参数 ----

struct RdmaPutArgs : rtest::bench::BaseArgs {
  std::uint64_t size{100ULL * 1024 * 1024};
  std::uint64_t count{10};
};

bool RdmaPutParseArgs(int argc, char** argv, RdmaPutArgs& a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto need = [&](std::string_view& val) -> bool {
      if (i + 1 >= argc) { std::cerr << "missing value for " << arg << "\n"; return false; }
      val = argv[++i];
      return true;
    };
    std::string_view val;
    if (arg == "--proxy") {
      if (!need(val)) return false;
      a.proxy = std::string(val);
    } else if (arg == "--size") {
      if (!need(val) || !rtest::ParseSize(val, a.size)) { std::cerr << "bad --size\n"; return false; }
    } else if (arg == "--count") {
      if (!need(val) || !rtest::bench::ParseUint(val, a.count)) { std::cerr << "bad --count\n"; return false; }
    } else if (arg == "--concurrency") {
      std::uint64_t v;
      if (!need(val) || !rtest::bench::ParseUint(val, v)) { std::cerr << "bad --concurrency\n"; return false; }
      a.concurrency = static_cast<std::uint32_t>(v);
    } else if (arg == "--warmup") {
      std::uint64_t v;
      if (!need(val) || !rtest::bench::ParseUint(val, v)) { std::cerr << "bad --warmup\n"; return false; }
      a.warmup = static_cast<std::uint32_t>(v);
    } else if (arg == "--bucket") {
      if (!need(val)) return false;
      a.bucket = std::string(val);
    } else if (arg == "--key-prefix") {
      if (!need(val)) return false;
      a.key_prefix = std::string(val);
    } else if (arg == "--verify-crc32c") {
      a.verify_crc32c = true;
    } else if (arg == "--trace") {
      a.trace = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: us3_turbo_bench_rdma_put [options]\n"
                << "  --proxy HOST:PORT     proxy endpoint (default " << rtest::kDefaultProxyEndpoint << ")\n"
                << "  --size N[K|M|G]        object size (default 100M)\n"
                << "  --count N              number of objects (default 10)\n"
                << "  --concurrency N        worker threads (default 1)\n"
                << "  --warmup N             warmup ops per worker (default 0)\n"
                << "  --bucket NAME          bucket (default test-bucket)\n"
                << "  --key-prefix STR       key prefix (default bench)\n"
                << "  --verify-crc32c        enable CRC32C verification\n"
                << "  --trace                log per-PUT stage latency\n";
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

void RdmaPutWorker(std::size_t wid, const RdmaPutArgs& a, us3_turbo::client::Client& client,
                   const std::vector<std::byte>& host_pattern, std::atomic<std::uint64_t>& next,
                   std::uint64_t total, std::barrier<StartSetter>& sync,
                   std::atomic<clk::time_point>& start, rtest::bench::WorkerStats& stats) {
  using namespace us3_turbo::client;

  // worker 独立 host buffer（拷贝 pattern）
  std::vector<std::byte> host(a.size);
  std::memcpy(host.data(), host_pattern.data(), a.size);
  stats.ready = true;

  ConstBufferView buf{.data = host.data(), .size = a.size};
  auto do_put = [&](const std::string& key) {
    ClientProxyPutRequest req;
    req.bucket = a.bucket;
    req.key = key;
    req.object_size = a.size;
    req.path = PutDataPath::kRdma;
    ClientProxyPutResponse out;
    auto t0 = clk::now();
    bool ok = client.PutObjectRdma(req, buf, out);
    auto t1 = clk::now();
    if (ok) {
      stats.rounds.push_back(
          RoundResult{.data_plane_ms = ms_double(t1 - t0).count(), .total_ms = ms_double(t1 - t0).count(),
                      .bytes = a.size, .ok = true});
      ++stats.ok;
      stats.bytes += a.size;
    } else {
      stats.rounds.push_back(RoundResult{.ok = false});
      ++stats.fail;
    }
  };

  // warmup
  for (std::uint64_t i = 0; i < a.warmup; ++i)
    do_put(a.key_prefix + "-warmup-" + std::to_string(wid) + "-" + std::to_string(i));

  // barrier 对齐起跑
  sync.arrive_and_wait();

  // 原子计数器领取任务
  std::uint64_t idx;
  while ((idx = next.fetch_add(1, std::memory_order_relaxed)) < total)
    do_put(a.key_prefix + "-" + std::to_string(idx));
  stats.end = clk::now();
}

// ---- main ----

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  RdmaPutArgs a;
  if (!RdmaPutParseArgs(argc, argv, a)) return 2;

  std::cout << "=== rdma PUT bench ===\n"
            << "  proxy       : " << a.proxy << "\n"
            << "  object size : " << rtest::HumanBytes(a.size) << "\n"
            << "  count       : " << a.count << "\n"
            << "  concurrency : " << a.concurrency << "\n"
            << "  warmup      : " << a.warmup << "\n"
            << "  bucket      : " << a.bucket << "\n"
            << "  key-prefix  : " << a.key_prefix << "\n"
            << "  verify-crc  : " << (a.verify_crc32c ? "on" : "off") << "\n"
            << std::endl;

  // 共享 host pattern
  std::vector<std::byte> host_pattern(a.size);
  rtest::FillHostPattern(host_pattern);

  // init
  Client client(ClientOptions{.endpoint = a.proxy, .verify_crc32c = a.verify_crc32c,
                               .latency_trace = a.trace});
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  // 启动 worker
  const std::size_t nworkers = static_cast<std::size_t>(a.concurrency);
  std::atomic<std::uint64_t> next{0};
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});

  std::vector<rtest::bench::WorkerStats> stats(nworkers);
  std::vector<std::thread> threads;
  threads.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w)
    threads.emplace_back(RdmaPutWorker, w, std::ref(a), std::ref(client), std::cref(host_pattern),
                         std::ref(next), a.count, std::ref(sync), std::ref(start), std::ref(stats[w]));
  for (auto& t : threads) t.join();

  // 结果报告
  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  rtest::bench::PrintPutGetResults("rdma", "PUT", stats, a.concurrency, t_start);

  client.Shutdown();
  for (const auto& s : stats)
    if (s.fail > 0) return 2;
  return 0;
}
