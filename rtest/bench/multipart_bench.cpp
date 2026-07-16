// multipart_bench.cpp — GDS / UCX 分段上传性能基准（rtest/bench）。
//
// 通过编译期宏选通路：
//   BENCH_GDS=1 → GDS（device 显存，链 CUDA；worker 各自 cudaMalloc + H2D）
//   BENCH_UCX=1 → UCX（host 内存，无 CUDA 依赖）
// 两者逻辑共享同一份 main，仅 buffer 类型与上传 API 不同。
//
// 测量维度：
//   1. 阶段耗时 —— 每轮记录 Create / ΣUploadPart / Complete 的 wall time，
//      拆出"数据面"（UploadPart，含 block 级 PutBlock×N）与"控制面"
//      （Create+Complete，即 dbgate 索引读写）。
//   2. 吞吐 —— 串行聚合吞吐 + 多 worker 并发聚合吞吐（共享 Client，brpc
//      channel 与内存管理器单例线程安全）。
//   3. 单轮时延分布（min/p50/p95/max）。
//   4. 可选 CSV 输出，便于横向对比 GDS vs UCX。
//
// 用法（GDS）：
//   us3_turbo_bench_gds_multipart \
//     --proxy 192.168.1.198:9100 --total 64M --part-size 16M \
//     --concurrency 1 --reps 5 [--csv]
// 用法（UCX）：
//   UCX_NET_DEVICES=mlx5_2:1 us3_turbo_bench_ucx_multipart \
//     --proxy 192.168.1.198:9100 --total 64M --part-size 16M --reps 5
//
// 并发压测：
//   --concurrency 8 --reps 3 --total 256M --part-size 16M
//
// 说明：
//   - part_size 默认 16M（= proxy multipart_part_size 上限）。非 last part 必须
//     恰好等于该值；当 total 不能被 part_size 整除时，最后一段 part < part_size，
//     由 proxy 在 Complete 时校验，符合 S3 语义。
//   - reps：每 worker 重复完整 multipart 上传的轮数，串行模式下即采样数。
//   - warmup：正式计时前的预热轮数（不计入统计），用于 token/descriptor 懒注册
//     与连接池预热，消除首轮冷启动偏差。

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#if defined(BENCH_GDS)
#include <cuda_runtime.h>
#endif

namespace {

using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

// ---- 编译期通路选择 ----
#if defined(BENCH_GDS)
constexpr char kPathName[] = "gds";
constexpr bool kIsGds = true;
#elif defined(BENCH_UCX)
constexpr char kPathName[] = "ucx";
constexpr bool kIsGds = false;
#else
#error "BENCH_GDS or BENCH_UCX must be defined"
#endif

// ---- 参数 ----
struct Args {
  std::string proxy{"192.168.1.198:9100"};
  std::uint64_t total{64ULL * 1024 * 1024};
  std::uint64_t part_size{16ULL * 1024 * 1024};
  std::uint32_t reps{5};
  std::uint32_t warmup{0};
  std::uint32_t concurrency{1};
  std::string bucket{"bench"};
  std::string key_prefix{"bench"};
  bool verify_crc32c{false};
  bool trace{false};
  bool csv{false};
};

// ---- 单轮 multipart 各阶段耗时 ----
struct RoundLatency {
  double create_ms{0};
  double upload_ms{0};  // Σ 所有 part 的 UploadPart
  double complete_ms{0};
  double total_ms{0};  // create + upload + complete
  std::uint64_t bytes{0};
  bool ok{false};
};

// 统计辅助：中位数与百分位。
double Median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t idx =
      std::min(v.size() - 1, static_cast<std::size_t>(p / 100.0 * (v.size() - 1)));
  return v[idx];
}

double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double s = 0.0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

// ---- 上传一轮 multipart，返回各阶段耗时 ----
// GDS: dev_buf 为 device 指针；UCX: host_buf 为 host 指针。两者互斥，由 kIsGds
// 在编译期裁剪未用分支，避免 UCX target 链接 CUDA。
RoundLatency RunOneRound(us3_turbo::client::Client& client, const Args& a, std::uint32_t round_idx,
                         std::uint32_t worker_idx, const auto& data_buf) {
  using namespace us3_turbo::client;
  RoundLatency lat;
  lat.bytes = a.total;

  const std::string key = a.key_prefix + "-" + kPathName + "-w" + std::to_string(worker_idx) +
                          "-r" + std::to_string(round_idx) + "-" + rtest::MakeTimestampSuffix();

  std::string upload_id, error;
  const auto t_create0 = clk::now();
  const PutDataPath path = kIsGds ? PutDataPath::kGds : PutDataPath::kUcx;
  if (!client.CreateMultipartUpload(a.bucket, key, path, upload_id, error)) {
    std::cerr << "[w" << worker_idx << " r" << round_idx
              << "] CreateMultipartUpload failed: " << error << "\n";
    return lat;
  }
  const auto t_create1 = clk::now();
  lat.create_ms = ms_double(t_create1 - t_create0).count();

  // 切 part：除最后一段外每段 == part_size；总大小 <= part_size 时退化为单 part。
  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((a.total + a.part_size - 1) / a.part_size);

  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);
  const auto t_up0 = clk::now();
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    const std::uint64_t off = static_cast<std::uint64_t>(i - 1) * a.part_size;
    const std::uint64_t len = std::min(a.part_size, a.total - off);
    std::string etag;
    bool ok = false;
#if defined(BENCH_GDS)
    ok = client.UploadPartGds(
        upload_id, i, ConstBufferView{.data = static_cast<char*>(data_buf) + off, .size = len},
        etag, error);
#else  // BENCH_UCX
    ok = client.UploadPartUcx(
        upload_id, i, ConstBufferView{.data = static_cast<std::byte*>(data_buf) + off, .size = len},
        etag, error);
#endif
    if (!ok) {
      std::cerr << "[w" << worker_idx << " r" << round_idx << "] UploadPart " << i
                << " failed: " << error << "\n";
      std::string abort_err;
      (void)client.AbortMultipartUpload(upload_id, abort_err);
      return lat;
    }
    parts.push_back({i, etag});
  }
  const auto t_up1 = clk::now();
  lat.upload_ms = ms_double(t_up1 - t_up0).count();

  Client::CompletedMultipart done;
  const auto t_cmp0 = clk::now();
  const bool ok = client.CompleteMultipartUpload(upload_id, parts, done);
  const auto t_cmp1 = clk::now();
  lat.complete_ms = ms_double(t_cmp1 - t_cmp0).count();

  if (!ok || done.object_size != a.total) {
    std::cerr << "[w" << worker_idx << " r" << round_idx << "] Complete failed: " << done.error
              << " size=" << done.object_size << "\n";
    return lat;
  }

  lat.total_ms = lat.create_ms + lat.upload_ms + lat.complete_ms;
  lat.ok = true;
  return lat;
}

// barrier 的 completion functor：最后一个到达的线程记录统一起跑时刻。
struct StartSetter {
  std::atomic<clk::time_point>* start;
  void operator()() const noexcept { start->store(clk::now(), std::memory_order_relaxed); }
};

struct WorkerStats {
  std::vector<RoundLatency> rounds;
  std::uint32_t ok{0};
  std::uint32_t fail{0};
  clk::time_point end{};
  bool ready{false};
};

// ---- worker：各自分配 buffer，barrier 对齐后跑 reps 轮 ----
void Worker(std::uint32_t wid, const Args& a, us3_turbo::client::Client& client,
            const std::vector<std::byte>& host_pattern, std::barrier<StartSetter>& sync,
            std::atomic<clk::time_point>& start, WorkerStats& stats) {
  using namespace us3_turbo::client;

  // buffer 分配：GDS 用 device 显存（H2D 填充 pattern），UCX 直接用 host。
#if defined(BENCH_GDS)
  void* dev = nullptr;
  if (cudaError_t e = cudaMalloc(&dev, a.total); e != cudaSuccess) {
    std::cerr << "[w" << wid << "] cudaMalloc(" << rtest::HumanBytes(a.total)
              << ") failed: " << cudaGetErrorString(e) << "\n";
    return;
  }
  if (cudaError_t e = cudaMemcpy(dev, host_pattern.data(), a.total, cudaMemcpyHostToDevice);
      e != cudaSuccess) {
    std::cerr << "[w" << wid << "] cudaMemcpy failed: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return;
  }
  stats.ready = true;
  void* data_buf = dev;
#else  // BENCH_UCX
  std::vector<std::byte> host(a.total);
  std::memcpy(host.data(), host_pattern.data(), a.total);
  stats.ready = true;
  void* data_buf = host.data();
#endif

  // warmup：不计入统计。
  for (std::uint32_t r = 0; r < a.warmup; ++r) {
    (void)RunOneRound(client, a, r, wid, data_buf);
  }

  // barrier 对齐起跑。
  sync.arrive_and_wait();
  const clk::time_point t_start = start.load(std::memory_order_relaxed);

  stats.rounds.reserve(a.reps);
  for (std::uint32_t r = 0; r < a.reps; ++r) {
    RoundLatency lat = RunOneRound(client, a, r, wid, data_buf);
    if (lat.ok) {
      ++stats.ok;
    } else {
      ++stats.fail;
    }
    stats.rounds.push_back(lat);
  }
  stats.end = clk::now();
  (void)t_start;

#if defined(BENCH_GDS)
  cudaFree(dev);
#else
  // host vector 析构释放。
#endif
}

// ---- 打印与 CSV ----
void PrintSummary(const Args& a, std::uint32_t ok, std::uint32_t fail, std::uint64_t total_bytes,
                  double wall_ms, const std::vector<RoundLatency>& all) {
  const double wall_s = wall_ms / 1000.0;
  const double tput =
      (wall_s > 0.0) ? static_cast<double>(total_bytes) / wall_s / (1024.0 * 1024.0) : 0.0;

  std::vector<double> create_ms, upload_ms, complete_ms, total_ms;
  create_ms.reserve(all.size());
  upload_ms.reserve(all.size());
  complete_ms.reserve(all.size());
  total_ms.reserve(all.size());
  for (const auto& r : all) {
    if (!r.ok) continue;
    create_ms.push_back(r.create_ms);
    upload_ms.push_back(r.upload_ms);
    complete_ms.push_back(r.complete_ms);
    total_ms.push_back(r.total_ms);
  }

  std::cout << "=== results (" << kPathName << " multipart, conc=" << a.concurrency
            << ", reps=" << a.reps << ") ===\n"
            << "  ok          : " << ok << "\n"
            << "  fail        : " << fail << "\n"
            << "  bytes       : " << rtest::HumanBytes(total_bytes) << "\n"
            << "  wall time   : " << wall_ms << " ms\n"
            << "  throughput  : " << tput << " MiB/s\n";
  if (!total_ms.empty()) {
    const double data_pct = (Mean(total_ms) > 0.0) ? Mean(upload_ms) / Mean(total_ms) * 100.0 : 0.0;
    std::cout << "  per-round(ms): \n"
              << "    create   : avg=" << Mean(create_ms) << "  p50=" << Median(create_ms)
              << "  p95=" << Percentile(create_ms, 95)
              << "  max=" << *std::max_element(create_ms.begin(), create_ms.end()) << "\n"
              << "    upload   : avg=" << Mean(upload_ms) << "  p50=" << Median(upload_ms)
              << "  p95=" << Percentile(upload_ms, 95)
              << "  max=" << *std::max_element(upload_ms.begin(), upload_ms.end())
              << "  (data-plane, " << data_pct << "% of round)\n"
              << "    complete : avg=" << Mean(complete_ms) << "  p50=" << Median(complete_ms)
              << "  p95=" << Percentile(complete_ms, 95)
              << "  max=" << *std::max_element(complete_ms.begin(), complete_ms.end()) << "\n"
              << "    total    : avg=" << Mean(total_ms) << "  p50=" << Median(total_ms)
              << "  p95=" << Percentile(total_ms, 95)
              << "  min=" << *std::min_element(total_ms.begin(), total_ms.end())
              << "  max=" << *std::max_element(total_ms.begin(), total_ms.end()) << "\n";
  }
}

void PrintCsv(const Args& a, const std::vector<RoundLatency>& all) {
  // 表头
  std::cout << "path,total_bytes,part_bytes,parts,concurrency,rep,"
               "create_ms,upload_ms,complete_ms,total_ms,ok\n";
  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((a.total + a.part_size - 1) / a.part_size);
  std::uint32_t rep = 0;
  for (const auto& r : all) {
    std::cout << kPathName << "," << a.total << "," << a.part_size << "," << num_parts << ","
              << a.concurrency << "," << rep++ << "," << r.create_ms << "," << r.upload_ms << ","
              << r.complete_ms << "," << r.total_ms << "," << (r.ok ? 1 : 0) << "\n";
  }
}

// ---- 参数解析 ----
bool ParseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        return false;
      }
      v = argv[++i];
      return true;
    };
    auto need_u32 = [&](std::uint32_t& v) -> bool {
      std::string s;
      if (!need(s)) return false;
      char* end = nullptr;
      unsigned long long n = std::strtoull(s.c_str(), &end, 10);
      if (end == s.c_str() || *end != '\0') {
        std::cerr << "bad value for " << arg << ": " << s << "\n";
        return false;
      }
      v = static_cast<std::uint32_t>(n);
      return true;
    };
    if (arg == "--proxy") {
      if (!need(a.proxy)) return false;
    } else if (arg == "--total") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, a.total)) {
        std::cerr << "bad --total\n";
        return false;
      }
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, a.part_size)) {
        std::cerr << "bad --part-size\n";
        return false;
      }
    } else if (arg == "--reps") {
      if (!need_u32(a.reps)) return false;
    } else if (arg == "--warmup") {
      if (!need_u32(a.warmup)) return false;
    } else if (arg == "--concurrency") {
      if (!need_u32(a.concurrency)) return false;
    } else if (arg == "--bucket") {
      if (!need(a.bucket)) return false;
    } else if (arg == "--key-prefix") {
      if (!need(a.key_prefix)) return false;
    } else if (arg == "--verify-crc32c") {
      a.verify_crc32c = true;
    } else if (arg == "--trace") {
      a.trace = true;
    } else if (arg == "--csv") {
      a.csv = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: us3_turbo_bench_" << kPathName << "_multipart [options]\n"
                << "  --proxy ADDR        proxy endpoint (default "
                   "192.168.1.198:9100)\n"
                << "  --total SIZE        total object size (default 64M)\n"
                << "  --part-size SIZE    part size (default 16M, <=16M)\n"
                << "  --reps N            reps per worker (default 5)\n"
                << "  --warmup N          warmup rounds (default 0)\n"
                << "  --concurrency N     workers (default 1)\n"
                << "  --bucket NAME       bucket (default bench)\n"
                << "  --key-prefix NAME   key prefix (default bench)\n"
                << "  --verify-crc32c     enable end-to-end crc32c\n"
                << "  --trace             enable client latency_trace\n"
                << "  --csv               emit CSV instead of summary\n";
      return false;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  if (a.part_size == 0 || a.total == 0) {
    std::cerr << "total and part-size must be > 0\n";
    return false;
  }
  if (a.reps == 0) {
    std::cerr << "reps must be > 0\n";
    return false;
  }
  if (a.concurrency == 0) {
    std::cerr << "concurrency must be > 0\n";
    return false;
  }
  // proxy 约束：part_size 上限 16M。非 last part 必须恰好 == part_size，因此
  // part_size 应为 16M（除非刻意测更小 part 触发 Complete 拒绝，这里不做）。
  if (a.part_size > 16ULL * 1024 * 1024) {
    std::cerr << "part-size " << rtest::HumanBytes(a.part_size)
              << " > 16M (proxy multipart_part_size)\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  Args a;
  if (!ParseArgs(argc, argv, a)) return 2;

  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((a.total + a.part_size - 1) / a.part_size);

  if (!a.csv) {
    std::cout << "=== " << kPathName << " multipart bench ===\n"
              << "  proxy       : " << a.proxy << "\n"
              << "  total       : " << rtest::HumanBytes(a.total) << "\n"
              << "  part size   : " << rtest::HumanBytes(a.part_size) << "\n"
              << "  parts/round : " << num_parts << "\n"
              << "  reps        : " << a.reps << "\n"
              << "  warmup      : " << a.warmup << "\n"
              << "  concurrency : " << a.concurrency << "\n"
              << "  bucket      : " << a.bucket << "\n"
              << "  key-prefix  : " << a.key_prefix << "\n"
              << "  verify-crc  : " << (a.verify_crc32c ? "on" : "off") << "\n"
              << "  trace       : " << (a.trace ? "on" : "off") << "\n"
              << std::endl;
  }

  // 共享 host pattern（只读，各 worker 拷贝）。
  std::vector<std::byte> host_pattern(a.total);
  rtest::FillHostPattern(host_pattern);

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
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});
  std::vector<WorkerStats> stats(nworkers);

  std::vector<std::thread> threads;
  threads.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w) {
    threads.emplace_back(Worker, static_cast<std::uint32_t>(w), std::ref(a), std::ref(client),
                         std::cref(host_pattern), std::ref(sync), std::ref(start),
                         std::ref(stats[w]));
  }
  for (auto& t : threads) t.join();

  // 聚合。
  std::uint32_t ok = 0, fail = 0;
  std::uint64_t total_bytes = 0;
  std::vector<RoundLatency> all;
  clk::time_point end_max{};
  bool any_ready = false;
  for (std::size_t w = 0; w < nworkers; ++w) {
    ok += stats[w].ok;
    fail += stats[w].fail;
    total_bytes += static_cast<std::uint64_t>(stats[w].ok) * a.total;
    all.insert(all.end(), stats[w].rounds.begin(), stats[w].rounds.end());
    end_max = std::max(end_max, stats[w].end);
    if (stats[w].ready) any_ready = true;
  }
  if (!any_ready) {
    std::cerr << "no worker ready (buffer alloc failed)\n";
    client.Shutdown();
    return 1;
  }
  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  const double wall_ms = ms_double(end_max - t_start).count();

  if (a.csv) {
    PrintCsv(a, all);
  } else {
    PrintSummary(a, ok, fail, total_bytes, wall_ms, all);
  }
  std::cout.flush();

  client.Shutdown();
  return fail == 0 ? 0 : 1;
}
