// gds_multipart_bench.cpp — GDS 分段上传性能基准。
//
// 测量: 阶段耗时 (Create/UploadPart/Complete)、吞吐、per-round 时延、可选 CSV 输出。
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
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
  std::uint64_t part_size{rtest::kDefaultPartSize};
};

// ---- 单轮 multipart 执行 ----

RoundResult RunOneRound(us3_turbo::client::Client& client, const Args& a, std::uint32_t round_idx,
                        std::uint32_t worker_idx, void* dev_buf) {
  using namespace us3_turbo::client;
  RoundResult lat;
  lat.bytes = a.total;

  const std::string key = a.key_prefix + "-" + kPathName + "-w" + std::to_string(worker_idx) +
                          "-r" + std::to_string(round_idx) + "-" + rtest::MakeTimestampSuffix();

  std::string upload_id, error;
  const auto t_create0 = clk::now();
  const PutDataPath path = PutDataPath::kGds;
  if (!client.CreateMultipartUpload(a.bucket, key, path, upload_id, error)) {
    lat.error = "CreateMultipartUpload failed: " + error;
    std::cerr << "[w" << worker_idx << " r" << round_idx << "] " << lat.error << "\n";
    return lat;
  }
  const auto t_create1 = clk::now();
  lat.setup_ms = ms_double(t_create1 - t_create0).count();

  // 切 part：除最后一段外每段 == part_size。
  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((a.total + a.part_size - 1) / a.part_size);

  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);
  const auto t_up0 = clk::now();
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    const std::uint64_t off = static_cast<std::uint64_t>(i - 1) * a.part_size;
    const std::uint64_t len = std::min(a.part_size, a.total - off);
    std::string etag;
    bool ok = client.UploadPartGds(
        upload_id, i, ConstBufferView{.data = static_cast<char*>(dev_buf) + off, .size = len},
        etag, error);
    if (!ok) {
      lat.error = "UploadPart " + std::to_string(i) + " failed: " + error;
      std::cerr << "[w" << worker_idx << " r" << round_idx << "] " << lat.error << "\n";
      std::string abort_err;
      (void)client.AbortMultipartUpload(upload_id, abort_err);
      return lat;
    }
    parts.push_back({i, etag});
  }
  const auto t_up1 = clk::now();
  lat.data_plane_ms = ms_double(t_up1 - t_up0).count();

  Client::CompletedMultipart done;
  const auto t_cmp0 = clk::now();
  const bool ok = client.CompleteMultipartUpload(upload_id, parts, done);
  const auto t_cmp1 = clk::now();
  lat.control_plane_ms = ms_double(t_cmp1 - t_cmp0).count();

  if (!ok || done.object_size != a.total) {
    lat.error = "Complete failed: " + done.error + " size=" + std::to_string(done.object_size);
    std::cerr << "[w" << worker_idx << " r" << round_idx << "] " << lat.error << "\n";
    return lat;
  }

  lat.total_ms = lat.setup_ms + lat.data_plane_ms + lat.control_plane_ms;
  lat.ok = true;
  return lat;
}

// ---- worker ----

struct WorkerStats {
  std::vector<RoundResult> rounds;
  std::uint32_t ok{0};
  std::uint32_t fail{0};
  clk::time_point end{};
  bool ready{false};
};

void Worker(std::uint32_t wid, const Args& a, us3_turbo::client::Client& client,
            const std::vector<std::byte>& host_pattern, std::barrier<StartSetter>& sync,
            std::atomic<clk::time_point>& start, WorkerStats& stats) {
  // buffer 分配：GDS 用 device 显存（H2D 填充 pattern）。
  void* dev = nullptr;
  cudaError_t e = cudaMalloc(&dev, a.total);
  if (e != cudaSuccess) {
    std::cerr << "[w" << wid << "] cudaMalloc(" << rtest::HumanBytes(a.total)
              << ") failed: " << cudaGetErrorString(e) << "\n";
    return;
  }
  e = cudaMemcpy(dev, host_pattern.data(), a.total, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    std::cerr << "[w" << wid << "] cudaMemcpy failed: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return;
  }
  stats.ready = true;
  void* data_buf = dev;

  // warmup：不计入统计。
  for (std::uint32_t r = 0; r < a.warmup; ++r) {
    (void)RunOneRound(client, a, r, wid, data_buf);
  }

  // barrier 对齐起跑。
  sync.arrive_and_wait();
  const clk::time_point t_start = start.load(std::memory_order_relaxed);

  stats.rounds.reserve(a.reps);
  for (std::uint32_t r = 0; r < a.reps; ++r) {
    RoundResult lat = RunOneRound(client, a, r, wid, data_buf);
    if (lat.ok) {
      ++stats.ok;
    } else {
      ++stats.fail;
    }
    stats.rounds.push_back(lat);
  }
  stats.end = clk::now();
  (void)t_start;

  cudaFree(dev);
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
                << "  --proxy ADDR        proxy endpoint (default " << "192.168.1.198:9100" << ")\n"
                << "  --total SIZE        total object size (default 64M)\n"
                << "  --part-size SIZE    part size (default 4M, <=4M)\n"
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
  if (a.part_size > 4ULL * 1024 * 1024) {
    std::cerr << "part-size " << rtest::HumanBytes(a.part_size)
              << " > 4M (backend MAX_VALUE_LENGTH)\n";
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
  std::vector<RoundResult> all;
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
    rtest::bench::WriteCsv(kPathName, a, all, num_parts);
  } else {
    rtest::bench::PrintReport("gds multipart, conc=" + std::to_string(a.concurrency) +
                                  ", reps=" + std::to_string(a.reps),
                              all, wall_ms);
  }
  std::cout.flush();

  client.Shutdown();
  return fail == 0 ? 0 : 1;
}
