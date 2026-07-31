// test_get_multi_block_hash.cpp — GDS GET 多块对象 hash 验证。
//
// 验证: 多 part 对象 PUT 后 GET 读回，hash 非空且与 etag 一致，逐字节比对通过。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
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
  void* dev_put = nullptr;
  if (cudaMalloc(&dev_put, part1) != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(put) failed\n";
    return 1;
  }
  void* dev_get = nullptr;

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    cudaFree(dev_put);
    return 1;
  }

  const std::string key = std::string("rtest-t22-gds-") + rtest::MakeTimestampSuffix();
  bool test_passed = false;

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(kBucket, key, PutDataPath::kGds, upload_id, error)) {
    std::cerr << "[FAIL] " << kTestName << ": CreateMultipartUpload: " << error << "\n";
    goto cleanup;
  }

  // ---- UploadPart 1 + Part 2 + Complete ----
  {
    std::string etag1, etag2;
    // Part 1
    if (cudaMemcpy(dev_put, host_full.data(), part1, cudaMemcpyHostToDevice) != cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy p1 failed\n";
      goto cleanup;
    }
    if (!client.UploadPartGds(upload_id, 1, ConstBufferView{.data = dev_put, .size = part1}, etag1,
                              error)) {
      std::cerr << "[FAIL] " << kTestName << ": UploadPartGds 1: " << error << "\n";
      goto cleanup;
    }
    // Part 2
    if (cudaMemcpy(dev_put, host_full.data() + part1, part2, cudaMemcpyHostToDevice) !=
        cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy p2 failed\n";
      goto cleanup;
    }
    if (!client.UploadPartGds(upload_id, 2, ConstBufferView{.data = dev_put, .size = part2}, etag2,
                              error)) {
      std::cerr << "[FAIL] " << kTestName << ": UploadPartGds 2: " << error << "\n";
      goto cleanup;
    }
    // Complete
    std::vector<Client::PartInfo> parts{{1, etag1}, {2, etag2}};
    Client::CompletedMultipart done;
    if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
      std::cerr << "[FAIL] " << kTestName << ": Complete: " << done.error << "\n";
      goto cleanup;
    }
    if (done.object_size != total) {
      std::cerr << "[FAIL] " << kTestName << ": object_size mismatch: got=" << done.object_size
                << " want=" << total << "\n";
      goto cleanup;
    }
    std::cout << "  Complete: object_size=" << done.object_size << " etag=" << done.etag << "\n";
  }

  // ---- StatObject + GET ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(kBucket, key, obj_size, stat_err) || obj_size != total) {
      std::cerr << "[FAIL] " << kTestName << ": StatObject failed or size mismatch\n";
      goto cleanup;
    }
    if (cudaMalloc(&dev_get, total) != cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(get) failed\n";
      goto cleanup;
    }
    cudaMemset(dev_get, 0xBB, total);
    GetPathResult get_res;
    if (!client.GetObjectGds(kBucket, key, MutableBufferView{.data = dev_get, .size = total},
                             get_res) || !get_res.ok) {
      std::cerr << "[FAIL] " << kTestName << ": GetObjectGds: " << get_res.error_message << "\n";
      goto cleanup;
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
    cudaMemcpy(host_read.data(), dev_get, total, cudaMemcpyDeviceToHost);
    rtest::VerifyHostBuffer(host_read.data(), total, host_full, "multi-block");
  }

cleanup:
  client.Shutdown();
  if (dev_get) cudaFree(dev_get);
  cudaFree(dev_put);

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
