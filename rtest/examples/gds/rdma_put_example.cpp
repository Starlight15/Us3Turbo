// rdma_put_example.cpp — RDMA PUT 端到端验证。
//
// 与 ucx_put_example 对应：用 host 内存走 RDMA 链路（底层 libibverbs RDMA CM）。
// client 创建 listener + 注册 MR → 生成 token → proxy RdmaPut → backend RDMA_READ。
//
// 单步用法：
//   us3_turbo_rdma_put_example --proxy 192.168.1.198:9100 [--size 100M]
//   [--verify-crc32c] [--concurrency 8] [--reps 50] [--trace]
//
// 分段用法（--multipart）：
//   us3_turbo_rdma_put_example --multipart --proxy 192.168.1.198:9100
//   [--size 256M] [--part-size 4M] [--concurrency 8] [--verify-crc32c] [--trace]

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
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
  std::uint32_t concurrency = 1;
  std::uint32_t reps = 1;
  bool trace = false;
  bool multipart = false;
  std::uint64_t part_size = 4ULL * 1024ULL * 1024ULL;  // 默认 4MB

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
    } else if (arg == "--multipart") {
      multipart = true;
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !ParseSize(v, part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
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

  // concurrency==1 且 reps==1 且非分段时走原单次路径
  if (!multipart && concurrency == 1 && reps == 1) {
    std::vector<std::byte> host(bytes);
    for (std::size_t i = 0; i < bytes; ++i) host[i] = static_cast<std::byte>(i % 251U);

    ClientProxyPutRequest req;
    req.bucket = "test-bucket";
    req.key = "obj-rdma-1";
    req.object_size = bytes;
    req.path = PutDataPath::kRdma;

    ClientProxyPutResponse resp;
    bool ok = client.PutObject(req, ConstBufferView{.data = host.data(), .size = bytes}, resp);

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

  // =========================================================================
  // 分段上传路径（--multipart）
  // =========================================================================
  if (multipart) {
    if (bytes < part_size) {
      std::cerr << "total size (" << bytes << ") < part_size (" << part_size << ")\n";
      return 2;
    }
    const std::uint32_t num_parts =
        static_cast<std::uint32_t>((bytes + part_size - 1) / part_size);

    // 准备数据：单块 host buffer 对应整对象（每 part 从中切片引用）
    std::vector<std::byte> host(bytes);
    for (std::size_t i = 0; i < bytes; ++i) host[i] = static_cast<std::byte>(i % 251U);

    // 创建分段上传会话
    std::string upload_id;
    std::string err;
    if (!client.CreateMultipartUpload("test-bucket", "obj-rdma-mp", PutDataPath::kRdma, upload_id,
                                      err)) {
      std::cerr << "CreateMultipartUpload FAILED: " << err << "\n";
      client.Shutdown();
      return 1;
    }
    std::cout << "upload_id=" << upload_id << " parts=" << num_parts
              << " part_size=" << part_size << " total=" << bytes << "\n";

    // 并发上传各 part
    std::atomic<std::uint64_t> ok_parts{0};
    std::atomic<std::uint64_t> fail_parts{0};
    std::atomic<std::uint64_t> total_bytes{0};
    std::barrier part_gate(static_cast<std::ptrdiff_t>(concurrency));

    auto part_worker = [&](std::uint32_t start_part) {
      part_gate.arrive_and_wait();

      for (std::uint32_t p = start_part; p < num_parts; p += concurrency) {
        const std::uint64_t offset = static_cast<std::uint64_t>(p) * part_size;
        const std::uint64_t sz =
            std::min(part_size, bytes - offset);

        std::string etag;
        std::string perr;
        ConstBufferView buf{host.data() + offset, sz};
        if (client.UploadPartRdma(upload_id, p + 1, buf, etag, perr)) {
          ok_parts.fetch_add(1, std::memory_order_relaxed);
          total_bytes.fetch_add(sz, std::memory_order_relaxed);
        } else {
          fail_parts.fetch_add(1, std::memory_order_relaxed);
          std::cerr << "[part=" << (p + 1) << "] UploadPartRdma FAILED: " << perr << "\n";
        }
      }
    };

    const auto t_begin = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    for (std::uint32_t t = 0; t < concurrency; ++t) threads.emplace_back(part_worker, t);
    for (auto& th : threads) th.join();

    if (fail_parts.load() > 0) {
      std::cerr << "some parts failed, aborting upload\n";
      std::string aerr;
      client.AbortMultipartUpload(upload_id, aerr);
      client.Shutdown();
      return 1;
    }

    // 完成分段上传
    Client::CompletedMultipart cmpl;
    if (!client.CompleteMultipartUpload(upload_id, {}, cmpl)) {
      std::cerr << "CompleteMultipartUpload FAILED: " << cmpl.error << "\n";
      client.Shutdown();
      return 1;
    }
    const auto t_end = std::chrono::steady_clock::now();

    client.Shutdown();

    const double sec = std::chrono::duration<double>(t_end - t_begin).count();
    const double mib = static_cast<double>(total_bytes.load()) / (1024.0 * 1024.0);
    std::cout << "multipart ok object_id=" << cmpl.object_id << " etag=" << cmpl.etag
              << " size=" << cmpl.object_size << " parts=" << ok_parts.load()
              << " wall_sec=" << sec << " throughput_MiBps=" << (sec > 0 ? mib / sec : 0.0)
              << "\n";
    return 0;
  }

  // 并发路径：concurrency 线程共享 client，每线程 reps 次
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
      req.key = "obj-rdma-c" + std::to_string(tid) + "-r" + std::to_string(r);
      req.object_size = bytes;
      req.path = PutDataPath::kRdma;

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
