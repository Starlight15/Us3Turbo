// test_put_single.cpp — RDMA 单步 PUT，多尺寸验证。
//
// 验证: 1K / 1M / 4M 三个尺寸下 bytes==size、etag 非空。host 内存，无 CUDA 依赖。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  // ---- 常量 ----
  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr const char* kTestName = "rdma_put_single";
  constexpr std::uint64_t kTestSizes[] = {
      1ULL * 1024,               // 1 KiB — 小对象
      1ULL * 1024 * 1024,        // 1 MiB — 典型
      4ULL * 1024 * 1024,        // 4 MiB — 边界
  };

  // ---- args ----
  std::string proxy = kProxy;
  std::string rdma_bind_ip;  // RDMA listener bind IP(空=env/fallback)
  bool verify_crc = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--rdma-bind-ip" && i + 1 < argc) {
      rdma_bind_ip = argv[++i];
    } else if (a == "--verify-crc32c") {
      verify_crc = true;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy      : " << proxy << "\n"
            << "  verify-crc : " << (verify_crc ? "on" : "off") << "\n"
            << "  test sizes : " << (sizeof(kTestSizes) / sizeof(kTestSizes[0])) << "\n";

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy, .verify_crc32c = verify_crc, .rdma_bind_ip = rdma_bind_ip});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  // ---- test ----
  int passed = 0, failed = 0;
  for (std::uint64_t size : kTestSizes) {
    const std::string key = std::string("rtest-t31-rdma-") + rtest::HumanBytes(size) + "-" +
                            rtest::MakeTimestampSuffix();

    // prepare host buffer
    std::vector<std::byte> host(size);
    rtest::FillHostPattern(host);

    // PUT
    ClientProxyPutRequest req;
    req.bucket = kBucket;
    req.key = key;
    req.object_size = size;
    req.path = PutDataPath::kRdma;

    ClientProxyPutResponse resp;
    if (!client.PutObjectRdma(req, ConstBufferView{.data = host.data(), .size = size}, resp)) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": PutObjectRdma returned false\n";
      ++failed;
      continue;
    }
    const auto& pr = resp.rdma_result.value();
    if (pr.bytes != size) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": bytes=" << pr.bytes << " expected=" << size << "\n";
      ++failed;
      continue;
    }
    if (pr.etag.empty()) {
      std::cerr << "[FAIL] " << kTestName << " size=" << rtest::HumanBytes(size)
                << ": etag is empty\n";
      ++failed;
      continue;
    }

    std::cout << "  size=" << rtest::HumanBytes(size) << " etag=" << pr.etag << " crc32c=0x"
              << std::hex << pr.crc32c << std::dec << " OK\n";
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
