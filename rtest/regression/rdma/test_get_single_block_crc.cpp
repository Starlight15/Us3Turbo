// test_get_single_block_crc.cpp — RDMA GET 单块对象 CRC 一致性。
//
// 验证: PUT 后 GET 读回，crc32c / hash / bytes_read 与 PUT 结果一致。host 内存，无 CUDA 依赖。

#include <cstdint>
#include <cstring>
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
  constexpr const char* kTestName = "rdma_get_single_block_crc";
  constexpr std::uint64_t kSinglePutMax = 4ULL * 1024 * 1024;

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t size = rtest::kDefaultPartSize / 2;  // 默认 2M（< 4M 单块）
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--size" && i + 1 < argc) {
      if (!rtest::ParseSize(argv[++i], size)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  if (size > kSinglePutMax) {
    std::cerr << "[FAIL] " << kTestName << ": size must be <= 4M, got " << rtest::HumanBytes(size)
              << "\n";
    return 2;
  }

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy : " << proxy << "\n"
            << "  size  : " << rtest::HumanBytes(size) << "\n";

  // ---- buffers ----
  std::vector<std::byte> host_put(size);
  rtest::FillHostPattern(host_put);

  std::vector<std::byte> host_get(size);
  std::memset(host_get.data(), 0xAA, size);

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  const std::string key = std::string("rtest-t21-rdma-") + rtest::MakeTimestampSuffix();
  bool test_passed = false;

  // ---- PUT ----
  std::uint32_t put_crc = 0;
  std::string put_etag;
  {
    ClientProxyPutRequest req;
    req.bucket = kBucket;
    req.key = key;
    req.object_size = size;
    req.path = PutDataPath::kRdma;

    ClientProxyPutResponse resp;
    if (!client.PutObjectRdma(req, ConstBufferView{.data = host_put.data(), .size = size}, resp)) {
      std::cerr << "[FAIL] " << kTestName << ": PutObjectRdma returned false\n";
      goto cleanup;
    }
    const auto& pr = resp.rdma_result.value();
    put_etag = pr.etag;
    put_crc = pr.crc32c;
    std::cout << "  PUT: bytes=" << pr.bytes << " etag=" << put_etag << " crc32c=0x" << std::hex
              << put_crc << std::dec << "\n";
  }

  // ---- StatObject ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(kBucket, key, obj_size, stat_err) || obj_size != size) {
      std::cerr << "[FAIL] " << kTestName << ": StatObject failed or size mismatch\n";
      goto cleanup;
    }
  }

  // ---- GET ----
  {
    GetPathResult get_res;
    if (!client.GetObjectRdma(kBucket, key,
                              MutableBufferView{.data = host_get.data(), .size = size}, get_res) ||
        !get_res.ok) {
      std::cerr << "[FAIL] " << kTestName << ": GetObjectRdma: " << get_res.error_message << "\n";
      goto cleanup;
    }
    std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
              << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";

    if (get_res.crc32c == 0) {
      std::cerr << "[FAIL] " << kTestName << ": crc32c == 0 (expected non-zero)\n";
    } else if (get_res.hash.empty()) {
      std::cerr << "[FAIL] " << kTestName << ": hash is empty\n";
    } else if (get_res.crc32c != put_crc) {
      std::cerr << "[FAIL] " << kTestName << ": crc32c mismatch\n";
    } else if (get_res.hash != put_etag) {
      std::cerr << "[FAIL] " << kTestName << ": hash != put.etag\n";
    } else if (get_res.bytes_read != size) {
      std::cerr << "[FAIL] " << kTestName << ": bytes_read mismatch\n";
    } else {
      test_passed = true;
    }

    rtest::VerifyHostBuffer(host_get.data(), size, host_put, "single-block");
  }

cleanup:
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
