// rdma_put_example.cpp — RDMA 单步 PUT 端到端验证（rtest/examples/rdma）。
//
// 用 host 内存走 RDMA 链路（底层 libibverbs RDMA CM）。
// client 创建 listener + 注册 MR → 生成 token → proxy RdmaPut → backend RDMA_READ。
//
// 用法：
//   us3_turbo_rdma_put_example --proxy 192.168.1.198:9100 [--size 100M]
//     [--verify-crc32c] [--concurrency 8] [--reps 50] [--trace]

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t bytes = 100ULL * 1024ULL * 1024ULL;
  bool verify = false;
  std::uint32_t concurrency = 1;
  std::uint32_t reps = 1;
  bool trace = false;

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
    if (arg == "--proxy") {
      if (!need(proxy_addr)) return 2;
    } else if (arg == "--size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, bytes)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else if (arg == "--concurrency") {
      std::string v;
      if (!need(v)) return 2;
      concurrency = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
      if (concurrency == 0) concurrency = 1;
    } else if (arg == "--reps") {
      std::string v;
      if (!need(v)) return 2;
      reps = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
      if (reps == 0) reps = 1;
    } else if (arg == "--trace") {
      trace = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: us3_turbo_rdma_put_example [options]\n"
                << "  --proxy HOST:PORT        proxy endpoint (default 192.168.1.198:9100)\n"
                << "  --size N[K|M|G]          object size (default 100M)\n"
                << "  --concurrency N          worker threads (default 1)\n"
                << "  --reps N                 repetitions per worker (default 1)\n"
                << "  --verify-crc32c          enable CRC32C verification\n"
                << "  --trace                  log per-PUT stage latency\n";
      return 0;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;
  opts.latency_trace = trace;

  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    return 1;
  }

  // 单次 PUT（concurrency=1, reps=1）
  if (concurrency == 1 && reps == 1) {
    std::vector<std::byte> host(bytes);
    rtest::FillHostPattern(host);

    ClientProxyPutRequest req;
    req.bucket = "test-bucket";
    req.key = "obj-rdma-1";
    req.object_size = bytes;
    req.path = PutDataPath::kRdma;

    ClientProxyPutResponse resp;
    bool ok = client.PutObjectRdma(req, ConstBufferView{.data = host.data(), .size = bytes}, resp);

    client.Shutdown();

    if (!ok) {
      std::cerr << "PutObject(kRdma) FAILED\n";
      return 1;
    }
    const auto& r = resp.rdma_result.value();
    std::cout << "OK bytes=" << r.bytes_written << " etag=" << r.etag << " crc32c=" << std::hex
              << r.crc32c << std::dec << "\n";
    return 0;
  }

  // 并发路径：concurrency 线程共享 client，每线程 reps 次
  std::atomic<std::uint64_t> ok_count{0};
  std::atomic<std::uint64_t> fail_count{0};
  std::barrier start_gate(static_cast<std::ptrdiff_t>(concurrency));

  auto worker = [&](std::uint32_t tid) {
    std::vector<std::byte> host(bytes);
    rtest::FillHostPattern(host);

    start_gate.arrive_and_wait();

    for (std::uint32_t r = 0; r < reps; ++r) {
      ClientProxyPutRequest req;
      req.bucket = "test-bucket";
      req.key = "obj-rdma-c" + std::to_string(tid) + "-r" + std::to_string(r);
      req.object_size = bytes;
      req.path = PutDataPath::kRdma;

      ClientProxyPutResponse resp;
      bool ok = client.PutObjectRdma(req, ConstBufferView{.data = host.data(), .size = bytes}, resp);
      if (ok) {
        ok_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        fail_count.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[tid=" << tid << " rep=" << r << "] PutObject FAILED\n";
      }
    }
  };

  const auto t_begin = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(concurrency);
  for (std::uint32_t t = 0; t < concurrency; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();
  const auto t_end = std::chrono::steady_clock::now();

  client.Shutdown();

  const double sec = std::chrono::duration<double>(t_end - t_begin).count();
  const std::uint64_t total_ok = ok_count.load();
  const double mib =
      static_cast<double>(total_ok) * static_cast<double>(bytes) / (1024.0 * 1024.0);
  std::cout << "concurrency=" << concurrency << " reps=" << reps << " size=" << bytes
            << " ok=" << total_ok << " fail=" << fail_count.load() << " wall_sec=" << sec
            << " throughput_MiBps=" << (sec > 0 ? mib / sec : 0.0) << "\n";

  return fail_count.load() == 0 ? 0 : 1;
}
