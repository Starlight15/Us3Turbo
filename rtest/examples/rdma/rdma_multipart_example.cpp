// rdma_multipart_example.cpp — RDMA 分段上传端到端验证（rtest/examples/rdma）。
//
// client → proxy(CreateMultipartUpload / UploadPartRdma / CompleteMultipartUpload)
//        → backend(PutBlock，通过 RDMA_READ 拉取各 part)。
// host 内存注册 MR → 生成 token → 并发上传各 part → Complete。
//
// 用法：
//   us3_turbo_rdma_multipart_example --proxy 192.168.1.198:9100
//     [--size 256M] [--part-size 4M] [--concurrency 8] [--verify-crc32c] [--trace]

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t bytes = 256ULL * 1024ULL * 1024ULL;
  std::uint64_t part_size = rtest::kDefaultPartSize;  // 默认 4M
  std::uint32_t concurrency = 4;
  bool verify = false;
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
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else if (arg == "--concurrency") {
      std::string v;
      if (!need(v)) return 2;
      concurrency = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
      if (concurrency == 0) concurrency = 1;
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else if (arg == "--trace") {
      trace = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: us3_turbo_rdma_multipart_example [options]\n"
                << "  --proxy HOST:PORT        proxy endpoint (default 192.168.1.198:9100)\n"
                << "  --size N[K|M|G]          total object size (default 256M)\n"
                << "  --part-size N[K|M|G]     part size (default 4M)\n"
                << "  --concurrency N          upload workers (default 4)\n"
                << "  --verify-crc32c          enable CRC32C verification\n"
                << "  --trace                  log per-stage latency\n";
      return 0;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  if (bytes < part_size) {
    std::cerr << "total size (" << rtest::HumanBytes(bytes)
              << ") < part_size (" << rtest::HumanBytes(part_size) << ")\n";
    return 2;
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

  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((bytes + part_size - 1) / part_size);

  // 准备数据：单块 host buffer 对应整对象（每 part 从中切片引用）
  std::vector<std::byte> host(bytes);
  rtest::FillHostPattern(host);

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
            << " part_size=" << rtest::HumanBytes(part_size)
            << " total=" << rtest::HumanBytes(bytes) << "\n";

  // 并发上传各 part，收集 etag 用于 CompleteMultipartUpload
  std::atomic<std::uint64_t> ok_parts{0};
  std::atomic<std::uint64_t> fail_parts{0};
  std::atomic<std::uint64_t> total_bytes{0};
  std::mutex parts_mutex;
  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);
  std::barrier part_gate(static_cast<std::ptrdiff_t>(concurrency));

  auto part_worker = [&](std::uint32_t start_part) {
    part_gate.arrive_and_wait();

    for (std::uint32_t p = start_part; p < num_parts; p += concurrency) {
      const std::uint64_t offset = static_cast<std::uint64_t>(p) * part_size;
      const std::uint64_t sz = std::min(part_size, bytes - offset);

      std::string etag;
      std::string perr;
      ConstBufferView buf{host.data() + offset, sz};
      if (client.UploadPartRdma(upload_id, p + 1, buf, etag, perr)) {
        ok_parts.fetch_add(1, std::memory_order_relaxed);
        total_bytes.fetch_add(sz, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(parts_mutex);
        parts.push_back({p + 1, etag});
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
    (void)client.AbortMultipartUpload(upload_id, aerr);
    client.Shutdown();
    return 1;
  }

  // 完成分段上传
  Client::CompletedMultipart cmpl;
  if (!client.CompleteMultipartUpload(upload_id, parts, cmpl)) {
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
