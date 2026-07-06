// multipart_bench_example.cpp — 单步 PUT vs 分段 PUT 性能对比（GDS 路径）。
//
// 对同一总大小（默认 20MB）跑 N 轮：先单步 GdsPut 一次，再 multipart
// （4 个 5MB part）一次，对比 wall time 与吞吐。无 gtest/benchmark 依赖，
// 沿用 gds_bench_example 的 plain-main 风格。
//
// 用法：
//   us3_turbo_multipart_bench_example \
//     --proxy 192.168.1.198:9100 --total 20M --reps 5
//
// 并发分段压测（CRC off 下分析瓶颈）：
//   us3_turbo_multipart_bench_example \
//     --total 256M --part-size 16M --concurrency 8 --multipart-only --reps 3

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "us3_turbo/client/client.h"

#include "client/src/common/request.h"

namespace {

using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

bool ParseSize(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t num = 0;
  std::size_t i = 0;
  for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
    num = num * 10 + static_cast<std::uint64_t>(s[i] - '0');
  }
  if (i == 0) return false;
  std::uint64_t mul = 1;
  if (i < s.size()) {
    if (i + 1 != s.size()) return false;
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

double Median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

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
  return std::string(buf);
}

// barrier 的 completion functor:最后一个到达的线程记录统一起跑时刻。
struct StartSetter {
  std::atomic<clk::time_point>* start;
  void operator()() const noexcept { start->store(clk::now(), std::memory_order_relaxed); }
};

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t total = 20ULL * 1024 * 1024;
  std::uint32_t reps = 5;
  std::uint64_t part_size = 5ULL * 1024 * 1024;
  std::uint64_t concurrency = 1;      // 并发分段上传 worker 数
  bool multipart_only = false;        // 跳过单步对比（允许 total > 16MiB 上限）

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) { std::cerr << "missing value for " << arg << "\n"; return false; }
      v = argv[++i]; return true;
    };
    if (arg == "--proxy") { if (!need(proxy_addr)) return 2; }
    else if (arg == "--total") {
      std::string v; if (!need(v) || !ParseSize(v, total)) { std::cerr << "bad --total\n"; return 2; }
    }
    else if (arg == "--part-size") {
      std::string v; if (!need(v) || !ParseSize(v, part_size)) { std::cerr << "bad --part-size\n"; return 2; }
    }
    else if (arg == "--reps") {
      std::string v; if (!need(v)) return 2;
      reps = static_cast<std::uint32_t>(std::strtoull(v.c_str(), nullptr, 10));
      if (reps == 0) { std::cerr << "bad --reps\n"; return 2; }
    }
    else if (arg == "--concurrency") {
      std::string v; if (!need(v)) return 2;
      concurrency = std::strtoull(v.c_str(), nullptr, 10);
      if (concurrency == 0) { std::cerr << "bad --concurrency\n"; return 2; }
    }
    else if (arg == "--multipart-only") { multipart_only = true; }
    else { std::cerr << "unknown arg: " << arg << "\n"; return 2; }
  }
  if (part_size == 0 || total < part_size) {
    std::cerr << "total must be >= part_size\n";
    return 2;
  }
  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((total + part_size - 1) / part_size);

  const bool run_single = !multipart_only;
  if (run_single && total > 16ULL * 1024 * 1024) {
    std::cerr << "total " << total << " > 16MiB single-step limit; "
                 "use --multipart-only\n";
    return 2;
  }

  std::cout << "=== multipart bench ===\n"
            << "  proxy    : " << proxy_addr << "\n"
            << "  total    : " << HumanBytes(total) << "\n"
            << "  part     : " << HumanBytes(part_size) << "\n"
            << "  parts    : " << num_parts << "\n"
            << "  reps     : " << reps << "\n"
            << "  conc     : " << concurrency << "\n"
            << "  mode     : " << (multipart_only ? "multipart-only" : "single vs multipart")
            << "\n" << std::endl;

  void* dev = nullptr;
  if (cudaError_t e = cudaMalloc(&dev, total); e != cudaSuccess) {
    std::cerr << "cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(total);
  for (std::size_t i = 0; i < total; ++i)
    host[i] = static_cast<std::byte>(i % 251U);
  cudaMemcpy(dev, host.data(), total, cudaMemcpyHostToDevice);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    cudaFree(dev);
    return 1;
  }

  // 串行模式（concurrency==1 且单步对比）：保留原单步 vs 分段逻辑。
  std::vector<double> single_ms, multi_ms;

  if (concurrency == 1 && !multipart_only) {
    for (std::uint32_t r = 0; r < reps; ++r) {
      // 单步 PUT（整 total）。
      {
        ClientProxyPutRequest req;
        req.bucket = "bench";
        req.key = "single-" + std::to_string(r);
        req.object_size = total;
        req.path = PutDataPath::kGds;
        ClientProxyPutResponse resp;
        const auto t0 = clk::now();
        const bool ok = client.PutObject(req,
            ConstBufferView{.data = dev, .size = total}, resp);
        const auto t1 = clk::now();
        if (!ok) {
          std::cerr << "single PutObject failed on rep " << r << "\n";
          cudaFree(dev);
          return 1;
        }
        single_ms.push_back(ms_double(t1 - t0).count());
      }
      // 分段 PUT（num_parts 个 part_size）。
      {
        std::string upload_id, error;
        if (!client.CreateMultipartUpload("bench", "multi-" + std::to_string(r),
                                          PutDataPath::kGds, upload_id, error)) {
          std::cerr << "CreateMultipartUpload failed: " << error << "\n";
          cudaFree(dev);
          return 1;
        }
        const auto t0 = clk::now();
        std::vector<Client::PartInfo> parts;
        for (std::uint32_t i = 1; i <= num_parts; ++i) {
          std::string etag;
          if (!client.UploadPartGds(upload_id, i,
              ConstBufferView{.data = dev, .size = part_size}, etag, error)) {
            std::cerr << "UploadPartGds " << i << " failed: " << error << "\n";
            cudaFree(dev);
            return 1;
          }
          parts.push_back({i, etag});
        }
        Client::CompletedMultipart done;
        if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
          std::cerr << "CompleteMultipartUpload failed: " << done.error << "\n";
          cudaFree(dev);
          return 1;
        }
        const auto t1 = clk::now();
        multi_ms.push_back(ms_double(t1 - t0).count());
      }
    }

    const double sm = Median(single_ms);
    const double mm = Median(multi_ms);
    const double s_mbs = (sm > 0.0) ? static_cast<double>(total) / (sm / 1000.0) / (1024.0 * 1024.0) : 0.0;
    const double m_mbs = (mm > 0.0) ? static_cast<double>(total) / (mm / 1000.0) / (1024.0 * 1024.0) : 0.0;
    std::cout << "=== results (median of " << reps << " reps) ===\n"
              << "  single    : " << sm << " ms  " << s_mbs << " MiB/s\n"
              << "  multipart : " << mm << " ms  " << m_mbs << " MiB/s\n";
    client.Shutdown();
    cudaFree(dev);
    return 0;
  }

  // 并发分段模式：nworkers 个 worker，每个跑 reps 轮完整 multipart 上传，
  // 共享统一起跑时刻。统计聚合吞吐与单轮时延。
  const std::size_t nworkers = static_cast<std::size_t>(concurrency);
  struct WStat {
    std::vector<double> lat_ms;
    std::uint64_t ok{0}, fail{0}, bytes{0};
    clk::time_point end{};
  };
  std::vector<WStat> stats(nworkers);
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});

  auto worker = [&](std::size_t wid) {
    // 每 worker 独立 device buffer（避免单 buffer 多线程并发注册冲突）。
    void* wdev = nullptr;
    if (cudaError_t e = cudaMalloc(&wdev, part_size); e != cudaSuccess) return;
    cudaMemcpy(wdev, dev, part_size, cudaMemcpyDeviceToDevice);
    ConstBufferView buf{.data = wdev, .size = part_size};

    sync.arrive_and_wait();
    for (std::uint32_t r = 0; r < reps; ++r) {
      std::string upload_id, error;
      if (!client.CreateMultipartUpload("bench",
          "conc-" + std::to_string(wid) + "-" + std::to_string(r),
          PutDataPath::kGds, upload_id, error)) {
        ++stats[wid].fail; continue;
      }
      const auto t0 = clk::now();
      std::vector<Client::PartInfo> parts;
      parts.reserve(num_parts);
      bool ok = true;
      for (std::uint32_t i = 1; i <= num_parts; ++i) {
        std::string etag;
        if (!client.UploadPartGds(upload_id, i, buf, etag, error)) { ok = false; break; }
        parts.push_back({i, etag});
      }
      if (ok) {
        Client::CompletedMultipart done;
        if (!client.CompleteMultipartUpload(upload_id, parts, done)) ok = false;
      }
      const auto t1 = clk::now();
      if (ok) {
        stats[wid].lat_ms.push_back(ms_double(t1 - t0).count());
        ++stats[wid].ok; stats[wid].bytes += total;
      } else {
        ++stats[wid].fail;
      }
    }
    stats[wid].end = clk::now();
    cudaFree(wdev);
  };

  std::vector<std::thread> th;
  th.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w) th.emplace_back(worker, w);
  for (auto& t : th) t.join();

  std::uint64_t ok = 0, fail = 0, bytes = 0;
  std::vector<double> lat;
  clk::time_point end_max{};
  for (std::size_t w = 0; w < nworkers; ++w) {
    ok += stats[w].ok; fail += stats[w].fail; bytes += stats[w].bytes;
    lat.insert(lat.end(), stats[w].lat_ms.begin(), stats[w].lat_ms.end());
    end_max = std::max(end_max, stats[w].end);
  }
  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  const double wall_ms = ms_double(end_max - t_start).count();
  const double wall_s = wall_ms / 1000.0;
  const double tput = (wall_s > 0.0) ? static_cast<double>(bytes) / wall_s / (1024.0 * 1024.0) : 0.0;
  std::sort(lat.begin(), lat.end());
  const double avg = lat.empty() ? 0.0 : [&]{ double s=0; for(double v:lat) s+=v; return s/lat.size(); }();

  std::cout << "=== results (concurrency=" << concurrency
            << ", reps=" << reps << ") ===\n"
            << "  ok         : " << ok << "\n"
            << "  fail       : " << fail << "\n"
            << "  bytes      : " << HumanBytes(bytes) << "\n"
            << "  wall time  : " << wall_ms << " ms\n"
            << "  throughput : " << tput << " MiB/s\n";
  if (!lat.empty()) {
    auto pct = [&](double p){ return lat[std::min(lat.size()-1, static_cast<size_t>(p/100.0*(lat.size()-1)))]; };
    std::cout << "  per-round(ms): avg=" << avg << "  min=" << lat.front()
              << "  p50=" << pct(50) << "  p95=" << pct(95) << "  max=" << lat.back() << "\n";
  }

  client.Shutdown();
  cudaFree(dev);
  return fail == 0 ? 0 : 2;
}
