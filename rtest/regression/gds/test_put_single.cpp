// test_put_single.cpp — GDS 单步 PUT，多尺寸验证。
//
// CASE: 1K/1M/4M 三种典型尺寸，验证 PUT 返回 bytes==size、etag 非空、
// StatObject 确认落盘。三个尺寸覆盖小对象、典型值、边界。

#include <cstdint>
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
  constexpr const char* kTestName = "gds_put_single";
  constexpr std::uint64_t kTestSizes[] = {
      1ULL * 1024,               // 1 KiB — 小对象
      1ULL * 1024 * 1024,        // 1 MiB — 典型
      4ULL * 1024 * 1024,        // 4 MiB — 边界
  };

  // ---- args ----
  std::string proxy = kProxy;
  bool verify_crc = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) proxy = argv[++i];
    else if (a == "--verify-crc32c") verify_crc = true;
    else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
  }

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy      : " << proxy << "\n"
            << "  verify-crc : " << (verify_crc ? "on" : "off") << "\n"
            << "  test sizes : 3\n";

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy, .verify_crc32c = verify_crc});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  int passed = 0, failed = 0;
  for (std::uint64_t size : kTestSizes) {
    const std::string key = std::string("rtest-t31-gds-") + rtest::HumanBytes(size) + "-" +
                            rtest::MakeTimestampSuffix();

    // alloc + fill GPU buffer
    rtest::DevMem dev(size);
    if (!dev.valid()) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": cudaMalloc failed\n";
      ++failed; continue;
    }
    std::vector<std::byte> host(size);
    rtest::FillHostPattern(host);
    if (cudaMemcpy(dev.get(), host.data(), size, cudaMemcpyHostToDevice) != cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": cudaMemcpy failed\n";
      ++failed; continue;
    }

    // PUT: 验证 bytes==size、etag 非空
    ClientProxyPutRequest req;
    req.bucket = kBucket;
    req.key = key;
    req.object_size = size;
    req.path = PutDataPath::kGds;

    ClientProxyPutResponse resp;
    if (!client.PutObjectGds(req, ConstBufferView{.data = dev.get(), .size = size}, resp)) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": PutObjectGds returned false\n";
      ++failed; continue;
    }

    const auto& pr = resp.gds_result.value();
    if (pr.bytes != size) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": bytes=" << pr.bytes << " expected=" << size << "\n";
      ++failed; continue;
    }
    if (pr.etag.empty()) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": etag is empty\n";
      ++failed; continue;
    }

    // StatObject: 确认最终落盘
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(kBucket, key, obj_size, stat_err) || obj_size != size) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": StatObject failed: got=" << obj_size << "\n";
      ++failed; continue;
    }

    std::cout << "  size=" << rtest::HumanBytes(size) << " etag=" << pr.etag
              << " crc32c=0x" << std::hex << pr.crc32c << std::dec << " OK\n";
    ++passed;
  }

  client.Shutdown();

  if (failed == 0) {
    std::cout << "[PASS] " << kTestName << " (" << passed << "/" << (passed + failed) << ")\n";
    return 0;
  }
  std::cerr << "[FAIL] " << kTestName << ": " << failed << "/" << (passed + failed)
            << " sizes failed\n";
  return 1;
}
