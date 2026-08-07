// test_get_multi_block_hash.cpp — GDS GET 多块对象 hash 验证。
//
// 验证: 多 part 对象 PUT 后 GET 读回，hash 非空且与 etag 一致，逐字节比对通过。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "rtest/cuda_guard.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  // ---- 常量 ----
  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr const char* kTestName = "gds_get_multi_block_hash";
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t total = 8ULL * 1024 * 1024;  // 默认 8M（> 4M 触发多块）
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--size" && i + 1 < argc) {
      if (!rtest::ParseSize(argv[++i], total)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  if (total <= kPartSize) {
    std::cerr << "[FAIL] " << kTestName << ": size must be > " << rtest::HumanBytes(kPartSize)
              << " for multi-block, got " << rtest::HumanBytes(total) << "\n";
    return 2;
  }
  const std::uint64_t part1 = kPartSize;
  const std::uint64_t part2 = total - kPartSize;

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy : " << proxy << "\n"
            << "  total : " << rtest::HumanBytes(total) << " (" << rtest::HumanBytes(part1) << " + "
            << rtest::HumanBytes(part2) << ")\n";

  // ---- 期望数据 ----
  std::vector<std::byte> host_full(total);
  {
    std::vector<std::byte> p1(part1);
    rtest::FillHostPattern(p1, 0);
    std::memcpy(host_full.data(), p1.data(), part1);
    std::vector<std::byte> p2(part2);
    rtest::FillHostPattern(p2, part1);
    std::memcpy(host_full.data() + part1, p2.data(), part2);
  }

  // ---- GPU buffers ----
  rtest::DevMem dev_put(part1);
  if (!dev_put.valid()) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(put) failed\n";
    return 1;
  }
  rtest::DevMem dev_get;

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }
  // RAII：作用域退出即 Shutdown。
  struct ShutdownGuard {
    Client* c;
    ~ShutdownGuard() { c->Shutdown(); }
  } shutdown_guard{&client};

  const std::string key = std::string("rtest-t22-gds-") + rtest::MakeTimestampSuffix();
  bool test_passed = false;

  // ---- CreateMultipartUpload ----
  std::string upload_id, trace_id, error;
  if (!client.CreateMultipartUpload(kBucket, key, PutDataPath::kGds, upload_id, trace_id, error)) {
    std::cerr << "[FAIL] " << kTestName << ": CreateMultipartUpload: " << error << "\n";
    return 1;
  }

  // ---- UploadPart 1 + Part 2 + Complete ----
  {
    std::string etag1, etag2;
    // Part 1
    if (cudaMemcpy(dev_put.get(), host_full.data(), part1, cudaMemcpyHostToDevice) != cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy p1 failed\n";
      return 1;
    }
    if (!client.UploadPartGds(upload_id, 1, ConstBufferView{.data = dev_put.get(), .size = part1},
                              etag1, error)) {
      std::cerr << "[FAIL] " << kTestName << ": UploadPartGds 1: " << error << "\n";
      return 1;
    }
    // Part 2
    if (cudaMemcpy(dev_put.get(), host_full.data() + part1, part2, cudaMemcpyHostToDevice) !=
        cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy p2 failed\n";
      return 1;
    }
    if (!client.UploadPartGds(upload_id, 2, ConstBufferView{.data = dev_put.get(), .size = part2},
                              etag2, error)) {
      std::cerr << "[FAIL] " << kTestName << ": UploadPartGds 2: " << error << "\n";
      return 1;
    }
    // Complete
    std::vector<Client::PartInfo> parts{{1, etag1}, {2, etag2}};
    Client::CompletedMultipart done;
    if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
      std::cerr << "[FAIL] " << kTestName << ": Complete: " << done.error << "\n";
      return 1;
    }
    if (done.object_size != total) {
      std::cerr << "[FAIL] " << kTestName << ": object_size mismatch: got=" << done.object_size
                << " want=" << total << "\n";
      return 1;
    }
    std::cout << "  Complete: object_size=" << done.object_size << " etag=" << done.etag << "\n";
  }

  // ---- StatObject + GET ----
  {
    std::uint64_t obj_size = 0;
    std::string get_trace_id, stat_err;
    if (!client.StatObject(kBucket, key, obj_size, get_trace_id, stat_err) || obj_size != total) {
      std::cerr << "[FAIL] " << kTestName << ": StatObject failed or size mismatch\n";
      return 1;
    }
    if (!dev_get.alloc(total)) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(get) failed\n";
      return 1;
    }
    cudaMemset(dev_get.get(), 0xBB, total);
    GetPathResult get_res;
    if (!client.GetObjectGds(kBucket, key, MutableBufferView{.data = dev_get.get(), .size = total},
                             get_res) || !get_res.ok) {
      std::cerr << "[FAIL] " << kTestName << ": GetObjectGds: " << get_res.error_message << "\n";
      return 1;
    }
    std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
              << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";

    if (get_res.crc32c != 0) {
      std::cerr << "[FAIL] " << kTestName << ": crc32c != 0 (expected 0 for multi-block)\n";
    } else if (get_res.hash.empty()) {
      std::cerr << "[FAIL] " << kTestName << ": hash is empty\n";
    } else if (get_res.bytes_read != total) {
      std::cerr << "[FAIL] " << kTestName << ": bytes_read mismatch\n";
    } else {
      test_passed = true;
    }

    // D2H 逐字节比对
    std::vector<std::byte> host_read(total);
    cudaMemcpy(host_read.data(), dev_get.get(), total, cudaMemcpyDeviceToHost);
    rtest::VerifyHostBuffer(host_read.data(), total, host_full, "multi-block");
  }

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
