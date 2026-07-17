// ucx_put_example.cpp — UCX PUT 端到端验证。
//
// 与 gds_put_example 对应：用 host 内存（非 device 显存）走 UCX 链路（底层
// RDMA）。 client → proxy → backend（backend ucp_get_nbx 反向拉 client host
// 内存）。
//
// 用法：
//   us3_turbo_ucx_put_example --proxy 192.168.1.198:9100 [--size 100M]
//   [--verify-crc32c] [--concurrency 8] [--reps 50] [--trace]

#include <algorithm>
#include <atomic>   // [诊断插桩]
#include <barrier>  // [诊断插桩]
#include <chrono>   // [诊断插桩]
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>  // [诊断插桩]
#include <vector>

#include "client/src/common/request.h"
#include "us3_turbo/client/client.h"

namespace {

bool ParseSize(const std::string& s, std::uint64_t& out) {
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
      case 'b':
        mul = 1ULL;
        break;
      case 'k':
        mul = 1024ULL;
        break;
      case 'm':
        mul = 1024ULL * 1024;
        break;
      case 'g':
        mul = 1024ULL * 1024 * 1024;
        break;
      default:
        return false;
    }
  }
  out = num * mul;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t bytes = 100ULL * 1024ULL * 1024ULL;
  bool verify = false;
  std::uint32_t concurrency = 1;  // [诊断插桩]
  std::uint32_t reps = 1;         // [诊断插桩]
  bool trace = false;             // [诊断插桩]

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
      if (!need(v) || !ParseSize(v, bytes)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else if (arg == "--concurrency") {  // [诊断插桩]
      std::string v;
      if (!need(v)) return 2;
      concurrency = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
      if (concurrency == 0) concurrency = 1;
    } else if (arg == "--reps") {  // [诊断插桩]
      std::string v;
      if (!need(v)) return 2;
      reps = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
      if (reps == 0) reps = 1;
    } else if (arg == "--trace") {  // [诊断插桩]
      trace = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;
  opts.latency_trace = trace;  // [诊断插桩]

  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    return 1;
  }

  // [诊断插桩] concurrency==1 且 reps==1 时走原单次路径，行为与改动前一致。
  if (concurrency == 1 && reps == 1) {
    std::vector<std::byte> host(bytes);
    for (std::size_t i = 0; i < bytes; ++i) host[i] = static_cast<std::byte>(i % 251U);

    ClientProxyPutRequest req;
    req.bucket = "test-bucket";
    req.key = "obj-ucx-1";
    req.object_size = bytes;
    req.path = PutDataPath::kUcx;

    ClientProxyPutResponse resp;
    bool ok = client.PutObject(req, ConstBufferView{.data = host.data(), .size = bytes}, resp);

    client.Shutdown();

    if (!ok) {
      std::cerr << "PutObject(kUcx) FAILED\n";
      return 1;
    }
    const auto& r = resp.ucx_result.value();
    std::cout << "OK bytes=" << r.bytes_written << " etag=" << r.etag << " crc32c=" << std::hex
              << r.crc32c << std::dec << "\n";
    return 0;
  }

  // [诊断插桩] 并发路径：concurrency 线程共享 client，每线程 reps 次，
  // 复用同一 buffer，std::barrier 对齐起跑。
  std::atomic<std::uint64_t> ok_count{0};
  std::atomic<std::uint64_t> fail_count{0};
  std::barrier start_gate(static_cast<std::ptrdiff_t>(concurrency));

  auto worker = [&](std::uint32_t tid) {
    std::vector<std::byte> host(bytes);
    for (std::size_t i = 0; i < bytes; ++i) host[i] = static_cast<std::byte>((i + tid) % 251U);

    start_gate.arrive_and_wait();

    for (std::uint32_t r = 0; r < reps; ++r) {
      ClientProxyPutRequest req;
      req.bucket = "test-bucket";
      req.key = "obj-ucx-c" + std::to_string(tid) + "-r" + std::to_string(r);
      req.object_size = bytes;
      req.path = PutDataPath::kUcx;

      ClientProxyPutResponse resp;
      bool ok = client.PutObject(req, ConstBufferView{.data = host.data(), .size = bytes}, resp);
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
