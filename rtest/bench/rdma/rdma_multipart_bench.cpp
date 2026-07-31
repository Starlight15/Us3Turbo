// rdma_multipart_bench.cpp — RDMA 分段上传性能基准。
//
// 测量: 阶段耗时 (Create/UploadPart/Complete)、吞吐、per-round 时延。可选 CSV 输出。

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

using rtest::bench::clk;
using rtest::bench::ms_double;
using rtest::bench::RoundResult;
using rtest::bench::StartSetter;

// ---- 参数 ----

struct RdmaMpArgs : rtest::bench::BaseArgs {
  std::uint64_t part_size{rtest::kDefaultPartSize};
};

// ---- 单轮 multipart ----

RoundResult RdmaMpRunOneRound(us3_turbo::client::Client& client, const RdmaMpArgs& a,
                              std::uint32_t round_idx, std::uint32_t worker_idx,
                              const std::vector<std::byte>& host_buf) {
  using namespace us3_turbo::client;
  RoundResult lat;
  lat.bytes = a.total;

  const std::string key = a.key_prefix + "-rdma-w" + std::to_string(worker_idx) +
                          "-r" + std::to_string(round_idx) + "-" + rtest::MakeTimestampSuffix();

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  const auto t0 = clk::now();
  if (!client.CreateMultipartUpload(a.bucket, key, PutDataPath::kRdma, upload_id, error)) {
    lat.error = "CreateMultipartUpload failed: " + error;
    std::cerr << "[w" << worker_idx << " r" << round_idx << "] " << lat.error << "\n";
    return lat;
  }
  lat.setup_ms = ms_double(clk::now() - t0).count();

  // ---- UploadPart × N ----
  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((a.total + a.part_size - 1) / a.part_size);
  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);

  const auto t_up0 = clk::now();
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    const std::uint64_t off = static_cast<std::uint64_t>(i - 1) * a.part_size;
    const std::uint64_t len = std::min(a.part_size, a.total - off);
    std::string etag;
    ConstBufferView buf{host_buf.data() + off, len};
    if (!client.UploadPartRdma(upload_id, i, buf, etag, error)) {
      lat.error = "UploadPartRdma " + std::to_string(i) + " failed: " + error;
      std::cerr << "[w" << worker_idx << " r" << round_idx << "] " << lat.error << "\n";
      client.AbortMultipartUpload(upload_id, error);
      return lat;
    }
    parts.push_back({i, etag});
  }
  lat.data_plane_ms = ms_double(clk::now() - t_up0).count();

  // ---- CompleteMultipartUpload ----
  Client::CompletedMultipart done;
  const auto t_cmp0 = clk::now();
  const bool ok = client.CompleteMultipartUpload(upload_id, parts, done);
  lat.control_plane_ms = ms_double(clk::now() - t_cmp0).count();

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

void RdmaMpWorker(std::uint32_t wid, const RdmaMpArgs& a, us3_turbo::client::Client& client,
                  const std::vector<std::byte>& host_pattern, std::barrier<StartSetter>& sync,
                  std::atomic<clk::time_point>& start, rtest::bench::WorkerStats& stats) {
  // worker 独立 host buffer（拷贝 pattern）
  std::vector<std::byte> host(a.total);
  std::memcpy(host.data(), host_pattern.data(), a.total);
  stats.ready = true;

  // warmup
  for (std::uint32_t r = 0; r < a.warmup; ++r)
    (void)RdmaMpRunOneRound(client, a, r, wid, host);

  // barrier
  sync.arrive_and_wait();

  // 正式测量
  stats.rounds.reserve(a.reps);
  for (std::uint32_t r = 0; r < a.reps; ++r) {
    RoundResult lat = RdmaMpRunOneRound(client, a, r, wid, host);
    if (lat.ok) { ++stats.ok; } else { ++stats.fail; }
    stats.rounds.push_back(lat);
  }
  stats.end = clk::now();
}

// ---- 参数解析 ----

bool RdmaMpParseArgs(int argc, char** argv, RdmaMpArgs& a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) { std::cerr << "missing value for " << arg << "\n"; return false; }
      v = argv[++i];
      return true;
    };
    auto need_u32 = [&](std::uint32_t& v) -> bool {
      std::string s;
      if (!need(s)) return false;
      return rtest::bench::ParseUint(s, reinterpret_cast<std::uint64_t&>(v));
    };
    if (arg == "--proxy") {
      if (!need(a.proxy)) return false;
    } else if (arg == "--total") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, a.total)) { std::cerr << "bad --total\n"; return false; }
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, a.part_size)) { std::cerr << "bad --part-size\n"; return false; }
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
      std::cout << "usage: us3_turbo_bench_rdma_multipart [options]\n"
                << "  --proxy ADDR        proxy endpoint\n"
                << "  --total SIZE        total object size (default 64M)\n"
                << "  --part-size SIZE    part size (default 4M, <=4M)\n"
                << "  --reps N            reps per worker (default 5)\n"
                << "  --warmup N          warmup rounds (default 0)\n"
                << "  --concurrency N     worker threads (default 1)\n"
                << "  --bucket NAME       bucket (default test-bucket)\n"
                << "  --key-prefix NAME   key prefix (default bench)\n"
                << "  --verify-crc32c     enable crc32c verification\n"
                << "  --trace             enable client latency_trace\n"
                << "  --csv               emit CSV instead of summary\n";
      return false;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  if (a.part_size == 0 || a.total == 0 || a.reps == 0 || a.concurrency == 0) {
    std::cerr << "total/part-size/reps/concurrency must be > 0\n";
    return false;
  }
  if (a.part_size > 4ULL * 1024 * 1024) {
    std::cerr << "part-size " << rtest::HumanBytes(a.part_size) << " > 4M (backend limit)\n";
    return false;
  }
  return true;
}

// ---- main ----

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  RdmaMpArgs a;
  if (!RdmaMpParseArgs(argc, argv, a)) return 2;

  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((a.total + a.part_size - 1) / a.part_size);

  if (!a.csv) {
    std::cout << "=== rdma multipart bench ===\n"
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

  // 共享 host pattern
  std::vector<std::byte> host_pattern(a.total);
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
  std::atomic<clk::time_point> start{clk::time_point{}};
  std::barrier<StartSetter> sync(static_cast<std::ptrdiff_t>(nworkers), StartSetter{&start});
  std::vector<rtest::bench::WorkerStats> stats(nworkers);

  std::vector<std::thread> threads;
  threads.reserve(nworkers);
  for (std::size_t w = 0; w < nworkers; ++w)
    threads.emplace_back(RdmaMpWorker, static_cast<std::uint32_t>(w), std::ref(a), std::ref(client),
                         std::cref(host_pattern), std::ref(sync), std::ref(start), std::ref(stats[w]));
  for (auto& t : threads) t.join();

  // 聚合
  std::vector<RoundResult> all;
  std::uint32_t total_ok = 0, total_fail = 0;
  clk::time_point end_max{};
  bool any_ready = false;
  for (auto& s : stats) {
    if (!s.ready) continue;
    any_ready = true;
    total_ok += s.ok;
    total_fail += s.fail;
    all.insert(all.end(), s.rounds.begin(), s.rounds.end());
    end_max = std::max(end_max, s.end);
  }
  if (!any_ready) {
    std::cerr << "no worker ready\n";
    client.Shutdown();
    return 1;
  }

  const clk::time_point t_start = start.load(std::memory_order_relaxed);
  const double wall_ms = ms_double(end_max - t_start).count();

  if (a.csv) {
    rtest::bench::WriteCsv("rdma", a, all, num_parts);
  } else {
    rtest::bench::PrintReport("rdma multipart", all, wall_ms);
  }

  client.Shutdown();
  return total_fail == 0 ? 0 : 1;
}
