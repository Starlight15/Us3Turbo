// rdma_get_bench.cpp — RDMA GET 性能基准。
//
// 流程: 先串行 PUT 全部对象，再并发 GET 测量读吞吐和时延。host 内存，无 CUDA 依赖。

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstring>
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

struct RdmaGetArgs : rtest::bench::BaseArgs {
  std::uint64_t size{4ULL * 1024 * 1024};
  std::uint64_t count{10};
};

bool RdmaGetParseArgs(int argc, char** argv, RdmaGetArgs& a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    std::string_view val;
    auto need = [&] { return rtest::bench::NeedVal(i, argc, argv, arg, val); };
    const int r = rtest::bench::ParseCommonArg(a, i, argc, argv, arg);
    if (r < 0) {
      std::cout << "usage: us3_turbo_bench_rdma_get [options]\n"
                << "  --proxy HOST:PORT     proxy endpoint (default " << rtest::kDefaultProxyEndpoint << ")\n"
                << "  --size N[K|M|G]        object size (default 4M)\n"
                << "  --count N              number of objects (default 10)\n"
                << "  --concurrency N        worker threads (default 1)\n"
                << "  --warmup N             warmup ops per worker (default 0)\n"
                << "  --bucket NAME          bucket (default test-bucket)\n"
                << "  --key-prefix STR       key prefix (default bench-get)\n"
                << "  --verify-crc32c        enable CRC32C verification\n"
                << "  --trace                log per-GET stage latency\n";
      return false;
    }
    if (r > 0) continue;
    if (arg == "--size") {
      if (!need() || !rtest::ParseSize(val, a.size)) { std::cerr << "bad --size\n"; return false; }
    } else if (arg == "--count") {
      if (!need() || !rtest::bench::ParseUint(val, a.count)) { std::cerr << "bad --count\n"; return false; }
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

void RdmaGetWorker(std::size_t wid, const RdmaGetArgs& a, us3_turbo::client::Client& client,
                   std::atomic<std::uint64_t>& next, std::uint64_t total,
                   std::barrier<StartSetter>& sync, std::atomic<clk::time_point>& start,
                   rtest::bench::WorkerStats& stats) {
  using namespace us3_turbo::client;

  // worker 独立 GET buffer
  std::vector<std::byte> get_buf(a.size);
  stats.ready = true;

  std::string trace_id;
  MutableBufferView buf{.data = get_buf.data(), .size = a.size};
  auto do_get = [&](const std::string& key) {
    GetPathResult res;
    auto t0 = clk::now();
    bool ok = client.GetObjectRdma(a.bucket, key, trace_id, buf, res);
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
    do_get(a.key_prefix + "-warmup-" + std::to_string(wid) + "-" + std::to_string(i));

  // barrier 对齐起跑
  sync.arrive_and_wait();

  // 原子计数器领取任务
  std::uint64_t idx;
  while ((idx = next.fetch_add(1, std::memory_order_relaxed)) < total)
    do_get(a.key_prefix + "-" + std::to_string(idx));
  stats.end = clk::now();
}

// ---- main ----

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  RdmaGetArgs a;
  a.key_prefix = "bench-get";
  if (!RdmaGetParseArgs(argc, argv, a)) return 2;

  std::cout << "=== rdma GET bench ===\n"
            << "  proxy       : " << a.proxy << "\n"
            << "  object size : " << rtest::HumanBytes(a.size) << "\n"
            << "  count       : " << a.count << "\n"
            << "  concurrency : " << a.concurrency << "\n"
            << "  warmup      : " << a.warmup << "\n"
            << "  bucket      : " << a.bucket << "\n"
            << "  key-prefix  : " << a.key_prefix << "\n"
            << std::endl;

  // 共享 host pattern（PUT 阶段用）
  std::vector<std::byte> host(a.size);
  rtest::FillHostPattern(host);

  // init
  Client client(ClientOptions{.endpoint = a.proxy, .latency_trace = a.trace});
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  // Phase 1: 串行 PUT 全部对象（确保 GET 阶段全部可读）
  std::cout << "Uploading " << a.count << " objects...\n";
  {
    ConstBufferView put_buf{.data = host.data(), .size = a.size};
    for (std::uint64_t i = 0; i < a.count; ++i) {
      ClientProxyPutRequest req;
      req.bucket = a.bucket;
      req.key = a.key_prefix + "-" + std::to_string(i);
      req.object_size = a.size;
      req.path = PutDataPath::kRdma;
      ClientProxyPutResponse resp;
      if (!client.PutObjectRdma(req, put_buf, resp)) {
        std::cerr << "PUT object " << i << " failed\n";
        client.Shutdown();
        return 1;
      }
    }
  }
  std::cout << "Upload done.\n\n";

  // Phase 2: 并发 GET 测量
  const std::size_t nworkers = static_cast<std::size_t>(a.concurrency);
  std::atomic<std::uint64_t> next{0};
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});

  std::vector<rtest::bench::WorkerStats> stats(nworkers);
  std::vector<std::thread> threads;
  threads.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w)
    threads.emplace_back(RdmaGetWorker, w, std::ref(a), std::ref(client), std::ref(next), a.count,
                         std::ref(sync), std::ref(start), std::ref(stats[w]));
  for (auto& t : threads) t.join();

  // 结果报告
  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  rtest::bench::PrintPutGetResults(stats, t_start);

  client.Shutdown();
  for (const auto& s : stats)
    if (s.fail > 0) return 2;
  return 0;
}
