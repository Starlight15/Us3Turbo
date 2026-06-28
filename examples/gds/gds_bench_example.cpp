// gds_bench_example.cpp — GDS PUT 基准测试。
//
// 可指定对象大小、数量、并发数,测量 GDS 上传的吞吐与时延。
//
// 用法:
//   us3_turbo_gds_bench_example \
//     --proxy 192.168.1.198:9100 \
//     --size 100M --count 100 --concurrency 8
//
// 模型:进程内共享一个 Client(PutObject 为 const,brpc channel 与
// GdsMemoryManager 单例均线程安全);每个 worker 线程拥有独立的 device
// buffer(各自 Register/Unregister),从共享原子计数器领取对象序号并发上传。

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "us3_turbo/client/client.h"

namespace {

using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

// ---- 参数 ----

struct Args {
  std::string   proxy{"192.168.1.198:9100"};
  std::uint64_t size{100ULL * 1024 * 1024};
  std::uint64_t count{10};
  std::uint64_t concurrency{1};
  std::uint64_t warmup{0};
  std::string   bucket{"test-bucket"};
  std::string   key_prefix{"obj"};
  bool          verify_crc32c{false};
  bool          trace{false};
};

// 支持 "100" / "100K" / "100M" / "1G"(大小写无关)。
bool ParseSize(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t num = 0;
  std::size_t   i   = 0;
  for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
    num = num * 10 + static_cast<std::uint64_t>(s[i] - '0');
  }
  if (i == 0) return false;  // 没有前导数字
  std::uint64_t mul = 1;
  if (i < s.size()) {
    if (i + 1 != s.size()) return false;  // 后缀必须是单个字符
    switch (std::tolower(static_cast<unsigned char>(s[i]))) {
      case 'b': mul = 1ULL; break;
      case 'k': mul = 1024ULL; break;
      case 'm': mul = 1024ULL * 1024; break;
      case 'g': mul = 1024ULL * 1024 * 1024; break;
      default: return false;
    }
  }
  out = num * mul;
  return true;
}

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

void PrintUsage() {
  std::cout <<
      "usage: us3_turbo_gds_bench_example [options]\n"
      "  --proxy HOST:PORT        control plane (proxy; GdsPut goes through it) (default 192.168.1.198:9100)\n"
      "  --size N[K|M|G]          object size     (default 100M)\n"
      "  --count N                number of objects (default 10)\n"
      "  --concurrency N          worker threads  (default 1)\n"
      "  --warmup N               warmup ops, not counted (default 0)\n"
      "  --bucket NAME            (default test-bucket)\n"
      "  --key-prefix STR         (default obj)\n"
      "  --verify-crc32c          enable client-side CRC32C verification\n"
      "  --trace                  log per-PUT stage latency (open/token/put)\n";
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
    if (arg == "--proxy") { if (!need(i, val)) return false; a.proxy = std::string(val); }
    else if (arg == "--size") { if (!need(i, val) || !ParseSize(val, a.size)) { std::cerr << "bad --size\n"; return false; } }
    else if (arg == "--count") { if (!need(i, val) || !ParseUint(val, a.count)) { std::cerr << "bad --count\n"; return false; } }
    else if (arg == "--concurrency") { if (!need(i, val) || !ParseUint(val, a.concurrency)) { std::cerr << "bad --concurrency\n"; return false; } }
    else if (arg == "--warmup") { if (!need(i, val) || !ParseUint(val, a.warmup)) { std::cerr << "bad --warmup\n"; return false; } }
    else if (arg == "--bucket") { if (!need(i, val)) return false; a.bucket = std::string(val); }
    else if (arg == "--key-prefix") { if (!need(i, val)) return false; a.key_prefix = std::string(val); }
    else if (arg == "--verify-crc32c") { a.verify_crc32c = true; }
    else if (arg == "--trace") { a.trace = true; }
    else if (arg == "--help" || arg == "-h") { PrintUsage(); std::exit(0); }
    else { std::cerr << "unknown arg: " << arg << "\n"; return false; }
  }
  if (a.size == 0 || a.count == 0 || a.concurrency == 0) {
    std::cerr << "size/count/concurrency must be > 0\n";
    return false;
  }
  return true;
}

// ---- 工具 ----

std::string HumanBytes(std::uint64_t b) {
  constexpr double K = 1024.0;
  char buf[64];
  if (b >= static_cast<std::uint64_t>(K * K * K))
    std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(b) / (K * K * K));
  else if (b >= static_cast<std::uint64_t>(K * K))
    std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(b) / (K * K));
  else if (b >= static_cast<std::uint64_t>(K))
    std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(b) / K);
  else
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
  return buf;
}

double Percentile(std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1));
  if (idx >= sorted.size()) idx = sorted.size() - 1;
  return sorted[idx];
}

// ---- worker ----

// barrier 的 completion functor:最后一个到达的线程记录统一起跑时刻。
struct StartSetter {
  std::atomic<clk::time_point>* start;
  void operator()() const noexcept { start->store(clk::now(), std::memory_order_relaxed); }
};

struct WorkerStats {
  std::vector<double> latencies_ms;  // 仅成功 op
  std::uint64_t       ok{0};
  std::uint64_t       fail{0};
  std::uint64_t       bytes{0};
  clk::time_point     end{};
  bool                ready{false};  // buffer 分配/注册成功
};

void Worker(std::size_t wid, const Args& a, us3_turbo::client::Client& client,
            const std::byte* host_pattern, std::atomic<std::uint64_t>& next,
            std::uint64_t total, std::barrier<StartSetter>& sync,
            std::atomic<clk::time_point>& start, WorkerStats& stats) {
  using namespace us3_turbo::client;

  // 1) 分配并填充 device buffer(每个 worker 独立)。
  void* dev = nullptr;
  if (cudaError_t e = cudaMalloc(&dev, a.size); e != cudaSuccess) {
    std::cerr << "[worker " << wid << "] cudaMalloc(" << HumanBytes(a.size)
              << ") failed: " << cudaGetErrorString(e) << "\n";
    return;
  }
  if (cudaError_t e = cudaMemcpy(dev, host_pattern, a.size, cudaMemcpyHostToDevice);
      e != cudaSuccess) {
    std::cerr << "[worker " << wid << "] cudaMemcpy failed: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return;
  }
  if (!client.RegisterDeviceBuffer(dev, a.size)) {
    std::cerr << "[worker " << wid << "] RegisterDeviceBuffer failed\n";
    cudaFree(dev);
    return;
  }
  stats.ready = true;

  ConstBufferView buf{.data = dev, .size = a.size};

  auto do_put = [&](const std::string& key) -> bool {
    PutObjectRequest  req;
    req.set_bucket(a.bucket).set_key(key).set_expected_size(a.size);
    TransferOutcome out;
    auto t0 = clk::now();
    bool ok  = client.PutObject(req, buf, out);
    auto t1  = clk::now();
    if (ok) {
      stats.latencies_ms.push_back(ms_double(t1 - t0).count());
      ++stats.ok;
      stats.bytes += a.size;
    } else {
      ++stats.fail;
    }
    return ok;
  };

  // 2) warmup(不计入统计,key 与正式对象隔离)。
  for (std::uint64_t i = 0; i < a.warmup; ++i) {
    do_put(a.key_prefix + "-warmup-" + std::to_string(wid) + "-" + std::to_string(i));
  }

  // 3) 屏障对齐后开始计时,所有 worker 共享同一个 start。
  sync.arrive_and_wait();
  // start 由 barrier 的 completion 函数写入(到达最后一个线程时)。

  // 4) 正式测量:从原子计数器领取序号直至 total。
  std::uint64_t idx;
  while ((idx = next.fetch_add(1, std::memory_order_relaxed)) < total) {
    do_put(a.key_prefix + "-" + std::to_string(idx));
  }
  stats.end = clk::now();

  (void)client.UnregisterDeviceBuffer(dev);
  cudaFree(dev);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  Args a;
  if (!ParseArgs(argc, argv, a)) return 1;

  std::cout << "=== GDS PUT bench ===\n"
            << "  proxy       : " << a.proxy << "\n"
            << "  object size : " << HumanBytes(a.size) << "\n"
            << "  count       : " << a.count << "\n"
            << "  concurrency : " << a.concurrency << "\n"
            << "  warmup      : " << a.warmup << "\n"
            << "  bucket      : " << a.bucket << "\n"
            << "  key-prefix  : " << a.key_prefix << "\n"
            << "  verify-crc32c: " << (a.verify_crc32c ? "on" : "off") << "\n"
            << std::endl;

  // 共享 host pattern(只读,各 worker 并发拷贝)。
  std::vector<std::byte> host(a.size);
  for (std::size_t i = 0; i < a.size; ++i) host[i] = static_cast<std::byte>(i % 251U);

  ClientOptions opts;
  opts.endpoint       = a.proxy;
  opts.verify_crc32c = a.verify_crc32c;
  opts.latency_trace = a.trace;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  const std::size_t       nworkers = static_cast<std::size_t>(a.concurrency);
  std::atomic<std::uint64_t> next{0};
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});

  std::vector<WorkerStats>    stats(nworkers);
  std::vector<std::thread>    threads;
  threads.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w) {
    threads.emplace_back(Worker, w, std::ref(a), std::ref(client), host.data(),
                         std::ref(next), a.count, std::ref(sync), std::ref(start),
                         std::ref(stats[w]));
  }
  for (auto& t : threads) t.join();

  // ---- 汇总 ----
  std::uint64_t ok = 0, fail = 0, bytes = 0;
  std::vector<double> lat;
  clk::time_point     end_min{}, end_max{};
  bool                any_ready = false;
  for (std::size_t w = 0; w < nworkers; ++w) {
    if (!stats[w].ready) {
      std::cerr << "[worker " << w << "] not ready (buffer alloc/register failed)\n";
      continue;
    }
    any_ready = true;
    ok += stats[w].ok;
    fail += stats[w].fail;
    bytes += stats[w].bytes;
    lat.insert(lat.end(), stats[w].latencies_ms.begin(), stats[w].latencies_ms.end());
    if (w == 0) { end_min = stats[w].end; end_max = stats[w].end; }
    else { end_min = std::min(end_min, stats[w].end); end_max = std::max(end_max, stats[w].end); }
  }

  if (!any_ready) {
    std::cerr << "no worker was ready — aborting\n";
    client.Shutdown();
    return 1;
  }

  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  const double          wall_ms = ms_double(end_max - t_start).count();
  const double          wall_s  = wall_ms / 1000.0;

  std::sort(lat.begin(), lat.end());
  double sum_lat = 0.0;
  for (double v : lat) sum_lat += v;
  const double avg_lat = lat.empty() ? 0.0 : sum_lat / static_cast<double>(lat.size());

  const double throughput_mbs = (wall_s > 0.0) ? static_cast<double>(bytes) / wall_s / (1024.0 * 1024.0) : 0.0;
  const double ops_per_sec    = (wall_s > 0.0) ? static_cast<double>(ok) / wall_s : 0.0;

  std::cout << "\n=== results ===\n"
            << "  ok           : " << ok << "\n"
            << "  fail         : " << fail << "\n"
            << "  bytes        : " << HumanBytes(bytes) << " (" << bytes << ")\n"
            << "  wall time    : " << wall_ms << " ms\n"
            << "  throughput   : " << throughput_mbs << " MiB/s  (" << ops_per_sec << " ops/s)\n";
  if (!lat.empty()) {
    std::cout << "  latency (ms) : avg=" << avg_lat
              << "  min=" << lat.front()
              << "  p50=" << Percentile(lat, 50.0)
              << "  p95=" << Percentile(lat, 95.0)
              << "  p99=" << Percentile(lat, 99.0)
              << "  max=" << lat.back() << "\n";
  }
  std::cout.flush();

  client.Shutdown();
  return fail == 0 ? 0 : 2;
}
