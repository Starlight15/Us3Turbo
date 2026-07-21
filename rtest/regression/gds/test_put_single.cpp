// test_put_single.cpp — T3.1 GDS 单步 PUT 验证
//
// 验证：不同对象大小（1K / 4M / 16M 边界）下 PutObjectGds 返回正确的
// bytes_written、etag 非空、StatObject 确认落盘且 size 一致。
// 可选 --verify-crc32c 对比 client 端 D2H 重算 CRC。
//
// 失败条件：PutObjectGds 返回 false、bytes_written != size、etag 为空、
// StatObject 失败或 size 不匹配。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {
constexpr char kTestName[] = "gds_put_single";
constexpr const char* kTestProxy = "192.168.1.198:9100";
constexpr const char* kTestBucket = "test-bucket";
constexpr std::uint64_t kTestSinglePutMaxBytes = 16ULL * 1024 * 1024;
constexpr std::uint64_t kSinglePutLimit = kTestSinglePutMaxBytes;

// 测试尺寸列表：覆盖小对象 / 典型 / 边界
constexpr std::uint64_t kTestSizes[] = {
    1ULL * 1024,                    // 1 KiB
    rtest::kDefaultPartSize,             // 4 MiB（典型）
    kSinglePutLimit,                // 16 MiB（单步上限）
};
}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = kTestProxy;
  bool verify_crc = false;

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
    } else if (arg == "--verify-crc32c") {
      verify_crc = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  const std::string bucket = kTestBucket;

  std::cout << "=== T3.1 GDS " << kTestName << " ===\n"
            << "  proxy      : " << proxy_addr << "\n"
            << "  verify-crc : " << (verify_crc ? "on" : "off") << "\n"
            << "  test sizes : 3\n";

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify_crc;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  int passed = 0;
  int failed = 0;

  for (std::uint64_t size : kTestSizes) {
    const std::string key = std::string("rtest-t31-gds-") + rtest::HumanBytes(size) +
                            "-" + rtest::MakeTimestampSuffix();

    // ---- allocate + fill GPU buffer ----
    void* dev = nullptr;
    cudaError_t e = cudaMalloc(&dev, size);
    if (e != cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": cudaMalloc: " << cudaGetErrorString(e) << "\n";
      ++failed;
      continue;
    }
    std::vector<std::byte> host(size);
    rtest::FillHostPattern(host);
    e = cudaMemcpy(dev, host.data(), size, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": cudaMemcpy: " << cudaGetErrorString(e) << "\n";
      cudaFree(dev);
      ++failed;
      continue;
    }

    // ---- PUT ----
    ClientProxyPutRequest req;
    req.bucket = bucket;
    req.key = key;
    req.object_size = size;
    req.path = PutDataPath::kGds;

    ClientProxyPutResponse resp;
    bool put_ok = client.PutObjectGds(req, ConstBufferView{.data = dev, .size = size}, resp);
    cudaFree(dev);

    if (!put_ok) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": PutObjectGds returned false\n";
      ++failed;
      continue;
    }
    const auto& pr = resp.gds_result.value();
    if (pr.bytes_written != size) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": bytes_written=" << pr.bytes_written << " expected=" << size << "\n";
      ++failed;
      continue;
    }
    if (pr.etag.empty()) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": etag is empty\n";
      ++failed;
      continue;
    }

    // ---- StatObject 确认落盘 ----
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) || obj_size != size) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": StatObject failed or size mismatch: got=" << obj_size
                << " err=" << stat_err << "\n";
      ++failed;
      continue;
    }

    std::cout << "  size=" << rtest::HumanBytes(size)
              << " etag=" << pr.etag
              << " crc32c=0x" << std::hex << pr.crc32c << std::dec << " OK\n";
    ++passed;
  }

  client.Shutdown();

  if (failed == 0) {
    std::cout << "[PASS] " << kTestName << " (" << passed << "/" << passed + failed << ")\n";
    return 0;
  }
  std::cerr << "[FAIL] " << kTestName << ": " << failed << "/" << passed + failed
            << " sizes failed\n";
  return 1;
}
